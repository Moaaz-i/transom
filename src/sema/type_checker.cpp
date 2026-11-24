#include "sema/type_checker.hpp"

#include <algorithm>
#include <unordered_map>

namespace transom {

namespace {
bool BuiltinTypeName(const std::string& n) {
    return n == "Int" || n == "Float" || n == "Bool" || n == "String" || n == "Bytes" ||
           n == "Void" || n == "List" || n == "Map" || n == "Result" || n == "Any";
}
} // namespace

TypeChecker::TypeChecker(DiagnosticEngine& diag, const PluginManager* plugins)
    : diag_(diag), plugins_(plugins) {
    registerBuiltins();
}

void TypeChecker::registerBuiltins() {
    functions_["abs"] = {{Type::floatT()}, Type::floatT(), false};
    functions_["round"] = {{Type::floatT()}, Type::intT(), false};
    functions_["min"] = {{Type::floatT(), Type::floatT()}, Type::floatT(), false};
    functions_["max"] = {{Type::floatT(), Type::floatT()}, Type::floatT(), false};
    functions_["clamp"] = {{Type::floatT(), Type::floatT(), Type::floatT()}, Type::floatT(), false};
    functions_["toString"] = {{Type::anyT()}, Type::stringT(), false};
    functions_["toInt"] = {{Type::anyT()}, Type::intT(), false};
    functions_["toFloat"] = {{Type::anyT()}, Type::floatT(), false};
    functions_["print"] = {{Type::anyT()}, Type::voidT(), false};

    // Polymorphic builtins checked specially
    functions_["len"] = {{}, Type::intT(), true};
    functions_["push"] = {{}, Type::voidT(), true};
    functions_["removeAt"] = {{}, Type::voidT(), true};
    functions_["clear"] = {{}, Type::voidT(), true};

    // System & Async stdlib builtins
    functions_["fsRead"] = {{Type::stringT()}, Type::stringT(), false};
    functions_["fsWrite"] = {{Type::stringT(), Type::stringT()}, Type::boolT(), false};
    functions_["fsExists"] = {{Type::stringT()}, Type::boolT(), false};
    functions_["storageGet"] = {{Type::stringT(), Type::stringT()}, Type::stringT(), false};
    functions_["storageSet"] = {{Type::stringT(), Type::stringT()}, Type::voidT(), false};
    functions_["clipboardCopy"] = {{Type::stringT()}, Type::voidT(), false};
    functions_["clipboardPaste"] = {{}, Type::stringT(), false};
    functions_["openUrl"] = {{Type::stringT()}, Type::voidT(), false};
    functions_["dialogOpenFile"] = {{}, Type::stringT(), false};
    functions_["dialogSaveFile"] = {{}, Type::stringT(), false};
    functions_["httpFetch"] = {{Type::stringT(), Type::stringT()}, Type::voidT(), false};
    functions_["jsonGet"] = {{Type::stringT(), Type::stringT()}, Type::stringT(), false};

    // First-class Bytes builtins
    functions_["bytesRead"] = {{Type::stringT()}, Type::bytesT(), false};
    functions_["bytesLen"] = {{Type::bytesT()}, Type::intT(), false};
    functions_["bytesAt"] = {{Type::bytesT(), Type::intT()}, Type::intT(), false};
    functions_["bytesSlice"] = {{Type::bytesT(), Type::intT(), Type::intT()}, Type::bytesT(), false};
    functions_["bytesToString"] = {{Type::bytesT()}, Type::stringT(), false};
    functions_["bytesToHex"] = {{Type::bytesT()}, Type::stringT(), false};

    // String utilities
    functions_["strTrim"] = {{Type::stringT()}, Type::stringT(), false};
    functions_["strSplit"] = {{Type::stringT(), Type::stringT()}, Type::listOf(Type::stringT()), false};
    functions_["strContains"] = {{Type::stringT(), Type::stringT()}, Type::boolT(), false};
    functions_["strStartsWith"] = {{Type::stringT(), Type::stringT()}, Type::boolT(), false};
    functions_["strReplace"] = {{Type::stringT(), Type::stringT(), Type::stringT()}, Type::stringT(), false};
    functions_["strCharAt"] = {{Type::stringT(), Type::intT()}, Type::stringT(), false};
    functions_["strSub"] = {{Type::stringT(), Type::intT(), Type::intT()}, Type::stringT(), false};

    // Hardened process execution (permission-gated to plugins with "process.exec:<cmd>")
    structs_["ExecResult"] = StructSig{"ExecResult", {
        {"exitCode", Type::intT()},
        {"stdout", Type::stringT()},
        {"stderr", Type::stringT()},
        {"timedOut", Type::boolT()},
    }};
    functions_["sysExec"] = {
        {Type::stringT(), Type::listOf(Type::stringT()), Type::intT(), Type::intT()},
        Type::structOf("ExecResult"),
        false
    };
    functions_["setTheme"] = {{Type::stringT()}, Type::boolT(), false};
    functions_["httpRequest"] = {
        {Type::stringT(), Type::stringT(), Type::stringT(), Type::stringT(), Type::stringT()},
        Type::voidT(),
        false
    };
    functions_["jsonPath"] = {{Type::stringT(), Type::stringT()}, Type::stringT(), false};

    // Map<String, T> builtins
    functions_["mapNew"] = {{}, Type::mapOf(Type::anyT()), false};
    functions_["hasKey"] = {{}, Type::boolT(), true};
    functions_["keys"] = {{}, Type::listOf(Type::stringT()), true};
    functions_["removeKey"] = {{}, Type::voidT(), true};
    functions_["range"] = {{Type::intT(), Type::intT()}, Type::listOf(Type::intT()), false};

    // Result<T> builtins
    functions_["Ok"] = {{}, Type::resultOf(Type::anyT()), true};
    functions_["Err"] = {{Type::stringT()}, Type::resultOf(Type::anyT()), false};
    functions_["isOk"] = {{}, Type::boolT(), true};
    functions_["isErr"] = {{}, Type::boolT(), true};
    functions_["unwrap"] = {{}, Type::anyT(), true};
    functions_["unwrapOr"] = {{}, Type::anyT(), true};
    functions_["errMsg"] = {{}, Type::stringT(), true};

    // Plugin event payload inspection inside widget action blocks
    functions_["eventName"] = {{}, Type::stringT(), false};
    functions_["eventPayload"] = {{}, Type::stringT(), false};

    // Collection & String pipeline utilities (pairs with |>)
    functions_["listJoin"] = {{}, Type::stringT(), true};
    functions_["listReverse"] = {{}, Type::listOf(Type::anyT()), true};
    functions_["listSort"] = {{}, Type::listOf(Type::anyT()), true};
    functions_["listContains"] = {{}, Type::boolT(), true};
    functions_["listSlice"] = {{}, Type::listOf(Type::anyT()), true};
    functions_["strUpper"] = {{Type::stringT()}, Type::stringT(), false};
    functions_["strLower"] = {{Type::stringT()}, Type::stringT(), false};

    for (auto& [_, sig] : functions_) {
        sig.minArgs = sig.params.size();
    }
}

std::optional<Type> TypeChecker::lookupVar(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return found->second;
    }
    return std::nullopt;
}

Type TypeChecker::resolveTypeRef(const TypeRef& ref) {
    if (ref.name == "Int") return Type::intT();
    if (ref.name == "Float") return Type::floatT();
    if (ref.name == "Bool") return Type::boolT();
    if (ref.name == "String") return Type::stringT();
    if (ref.name == "Bytes") return Type::bytesT();
    if (ref.name == "Void") return Type::voidT();
    if (ref.name == "List") {
        if (ref.typeArgs.size() != 1) {
            diag_.error(ref.span, "List requires 1 type argument, e.g. List<String>");
            return Type::listOf(Type::anyT());
        }
        return Type::listOf(resolveTypeRef(ref.typeArgs[0]));
    }
    if (ref.name == "Map") {
        if (ref.typeArgs.size() == 1) {
            return Type::mapOf(resolveTypeRef(ref.typeArgs[0]));
        }
        if (ref.typeArgs.size() == 2) {
            Type kt = resolveTypeRef(ref.typeArgs[0]);
            if (!kt.isError() && kt.kind != TypeKind::String) {
                diag_.error(ref.span, "Map keys must be String, e.g. Map<String, Int>");
            }
            return Type::mapOf(resolveTypeRef(ref.typeArgs[1]));
        }
        diag_.error(ref.span, "Map requires 1 or 2 type arguments, e.g. Map<String, Int>");
        return Type::mapOf(Type::anyT());
    }
    if (ref.name == "Result") {
        if (ref.typeArgs.empty() || ref.typeArgs.size() > 2) {
            diag_.error(ref.span, "Result requires 1 type argument, e.g. Result<Int>");
            return Type::resultOf(Type::anyT());
        }
        return Type::resultOf(resolveTypeRef(ref.typeArgs[0]));
    }
    if (enums_.count(ref.name)) {
        return Type::enumOf(ref.name);
    }
    if (structs_.count(ref.name)) {
        return Type::structOf(ref.name);
    }
    if (ref.name == "Num" || ref.name == "Number" || ref.name == "num" || ref.name == "number") {
        diag_.error(ref.span, "Unknown type '" + ref.name + "' (did you mean 'Int' or 'Float'?)");
    } else if (ref.name == "int" || ref.name == "integer" || ref.name == "Integer") {
        diag_.error(ref.span, "Unknown type '" + ref.name + "' (did you mean 'Int'?)");
    } else if (ref.name == "float" || ref.name == "double" || ref.name == "Double") {
        diag_.error(ref.span, "Unknown type '" + ref.name + "' (did you mean 'Float'?)");
    } else if (ref.name == "bool" || ref.name == "boolean" || ref.name == "Boolean") {
        diag_.error(ref.span, "Unknown type '" + ref.name + "' (did you mean 'Bool'?)");
    } else if (ref.name == "string" || ref.name == "str" || ref.name == "Str") {
        diag_.error(ref.span, "Unknown type '" + ref.name + "' (did you mean 'String'?)");
    } else if (ref.name == "void") {
        diag_.error(ref.span, "Unknown type 'void' (did you mean 'Void'?)");
    } else {
        diag_.error(ref.span, "Unknown type '" + ref.name + "'");
    }
    return Type::error();
}

CheckedProgram TypeChecker::check(const Program& program) {
    CheckedProgram out;
    styleSheet_ = &program.styleSheet;

    // 0. Check all .tplug official language modules first (contract, exports & capability security)
    for (const auto& pl : program.tplugs) {
        checkPlugin(pl);
    }

    // 0b. Register enums (closed sets of named variants; runtime value is the variant name)
    for (const auto& ed : program.enums) {
        if (enums_.count(ed.name) || BuiltinTypeName(ed.name)) {
            diag_.error(ed.span, "Duplicate or reserved enum name '" + ed.name + "'");
            continue;
        }
        if (ed.variants.empty()) diag_.error(ed.span, "Enum '" + ed.name + "' must declare at least one variant");
        std::vector<std::string> seen;
        for (size_t i = 0; i < ed.variants.size(); ++i) {
            if (std::find(seen.begin(), seen.end(), ed.variants[i]) != seen.end()) {
                diag_.error(ed.variantSpans[i], "Duplicate variant '" + ed.variants[i] + "' in enum '" + ed.name + "'");
            }
            seen.push_back(ed.variants[i]);
        }
        enums_[ed.name] = ed.variants;
    }

    // 1. Register struct names first (supports self/cross references)
    for (const auto& sd : program.structs) {
        if (structs_.count(sd.name)) {
            diag_.error(sd.span, "Duplicate struct '" + sd.name + "'");
            continue;
        }
        structs_[sd.name] = StructSig{sd.name, {}};
    }
    for (const auto& sd : program.structs) {
        auto& sig = structs_[sd.name];
        for (const auto& f : sd.fields) {
            sig.fields[f.name] = resolveTypeRef(f.type);
        }
    }

    // 2. Register top-level function signatures & components
    for (const auto& fn : program.functions) {
        FnSig sig;
        bool sawDefault = false;
        for (const auto& p : fn.params) {
            Type pt = resolveTypeRef(p.type);
            sig.params.push_back(pt);
            if (p.defaultValue) {
                sawDefault = true;
            } else {
                if (sawDefault) {
                    diag_.error(p.span, "Required parameter '" + p.name + "' cannot follow a parameter with a default value");
                }
                sig.minArgs++;
            }
        }
        sig.returnType = fn.returnType ? resolveTypeRef(*fn.returnType) : Type::voidT();
        functions_[fn.name] = sig;
    }

    for (const auto& comp : program.components) {
        if (components_.count(comp.name) || widgetRegistry().find(comp.name)) {
            diag_.error(comp.span, "Duplicate or reserved component name '" + comp.name + "'");
            continue;
        }
        components_[comp.name] = &comp;
    }

    // Check top-level constants and functions
    pushScope();
    for (const auto& cd : program.consts) {
        if (constVars_.count(cd.name)) {
            diag_.error(cd.span, "Duplicate const '" + cd.name + "'");
            continue;
        }
        Type vt = checkExpr(*cd.value);
        if (cd.typeAnnot) {
            Type at = resolveTypeRef(*cd.typeAnnot);
            if (!isAssignable(at, vt)) {
                diag_.error(cd.span, "Cannot initialize const '" + cd.name + "' of type " + at.name() + " with " + vt.name());
            }
            vt = at;
        }
        constVars_.insert(cd.name);
        defineVar(cd.name, vt);
    }
    for (const auto& fn : program.functions) {
        checkFn(fn);
    }

    // Check components
    for (const auto& comp : program.components) {
        pushScope();
        for (const auto& p : comp.params) {
            Type pt = resolveTypeRef(p.type);
            if (p.defaultValue) {
                Type dt = checkExpr(*p.defaultValue);
                if (!isAssignable(pt, dt)) {
                    diag_.error(p.span, "Default value for component param '" + p.name + "' expects " + pt.name() + ", got " + dt.name());
                }
            }
            defineVar(p.name, pt);
        }
        inComponent_ = true;
        if (comp.view) checkViewNode(*comp.view);
        inComponent_ = false;
        popScope();
    }

    // Check app declaration
    if (program.app) {
        const AppDecl& app = *program.app;
        checkWindowProps(app.windowProps);

        pushScope();

        for (const auto& fn : app.functions) {
            FnSig sig;
            bool sawDefault = false;
            for (const auto& p : fn.params) {
                Type pt = resolveTypeRef(p.type);
                sig.params.push_back(pt);
                if (p.defaultValue) {
                    sawDefault = true;
                } else {
                    if (sawDefault) {
                        diag_.error(p.span, "Required parameter '" + p.name + "' cannot follow a parameter with a default value");
                    }
                    sig.minArgs++;
                }
            }
            sig.returnType = fn.returnType ? resolveTypeRef(*fn.returnType) : Type::voidT();
            functions_[fn.name] = sig;
        }

        for (const auto& st : app.states) {
            if (stateVars_.count(st.name)) {
                diag_.error(st.span, "Duplicate state variable '" + st.name + "'");
                continue;
            }
            Type initType = checkExpr(*st.init);
            Type finalType = initType;
            if (st.typeAnnot) {
                Type annotType = resolveTypeRef(*st.typeAnnot);
                if (!isAssignable(annotType, initType)) {
                    diag_.error(st.span,
                                "Cannot initialize state '" + st.name + "' of type " +
                                    annotType.name() + " with " + initType.name());
                }
                finalType = annotType;
            }
            stateVars_.insert(st.name);
            defineVar(st.name, finalType);
            out.stateTypes[st.name] = finalType;
        }

        for (const auto& dr : app.deriveds) {
            if (stateVars_.count(dr.name) || derivedVars_.count(dr.name)) {
                diag_.error(dr.span, "Duplicate reactive signal '" + dr.name + "'");
                continue;
            }
            std::unordered_set<std::string> deps;
            activeDeps_ = &deps;
            Type exprType = checkExpr(*dr.expr);
            activeDeps_ = nullptr;

            Type finalType = exprType;
            if (dr.typeAnnot) {
                Type annotType = resolveTypeRef(*dr.typeAnnot);
                if (!isAssignable(annotType, exprType)) {
                    diag_.error(dr.span,
                                "Cannot initialize derived '" + dr.name + "' of type " +
                                    annotType.name() + " with " + exprType.name());
                }
                finalType = annotType;
            }
            derivedVars_.insert(dr.name);
            defineVar(dr.name, finalType);
            out.derivedTypes[dr.name] = finalType;
            out.derivedDeps[dr.name] = std::move(deps);
        }

        for (const auto& fn : app.functions) {
            checkFn(fn);
        }

        for (const auto& tm : app.timers) {
            Type it = checkExpr(*tm.intervalMs);
            if (!it.isError() && it.kind != TypeKind::Int) {
                diag_.error(tm.intervalMs->span, "Timer interval must be Int (milliseconds)");
            }
            pushScope();
            for (const auto& s : tm.body) checkStmt(*s);
            popScope();
        }

        if (!app.view) {
            diag_.error(app.span, "App '" + app.title + "' is missing a 'view' declaration");
        } else {
            checkViewNode(*app.view);
        }

        popScope();
    }

    popScope();
    return out;
}

bool TypeChecker::pluginHasPerm(const PluginDecl& pl, const std::string& perm) const {
    for (const auto& p : pl.permissions) {
        if (p == perm) return true;
    }
    return false;
}

void TypeChecker::checkCapabilityCall(const std::string& callee,
                                      const std::vector<ExprPtr>& args,
                                      Span span) {
    if (callee == "sysExec") {
        if (!activePlugin_) {
            diag_.error(span,
                        "SecurityError: sysExec() is only allowed inside a .tplug plugin with 'process.exec:<cmd>' permission");
            return;
        }
        if (!args.empty()) {
            if (auto* lit = std::get_if<StringLitExpr>(&args[0]->node)) {
                std::string needed = "process.exec:" + lit->value;
                if (!pluginHasPerm(*activePlugin_, needed)) {
                    diag_.error(span,
                                "SecurityError: plugin '" + activePlugin_->name +
                                    "' lacks required permission '" + needed + "'");
                }
            } else {
                bool hasAny = false;
                for (const auto& p : activePlugin_->permissions) {
                    if (p.rfind("process.exec:", 0) == 0) hasAny = true;
                }
                if (!hasAny) {
                    diag_.error(span,
                                "SecurityError: plugin '" + activePlugin_->name +
                                    "' lacks any 'process.exec:<cmd>' permission");
                }
            }
        }
        return;
    }

    if (!activePlugin_) return; // App-level fs/http calls are allowed directly

    if (callee == "fsRead" || callee == "fsExists" || callee == "bytesRead") {
        if (!pluginHasPerm(*activePlugin_, "fs.read")) {
            diag_.error(span,
                        "SecurityError: plugin '" + activePlugin_->name +
                            "' calls " + callee + "() without 'fs.read' permission");
        }
    } else if (callee == "fsWrite") {
        if (!pluginHasPerm(*activePlugin_, "fs.write")) {
            diag_.error(span,
                        "SecurityError: plugin '" + activePlugin_->name +
                            "' calls fsWrite() without 'fs.write' permission");
        }
    } else if (callee == "httpFetch") {
        if (!pluginHasPerm(*activePlugin_, "net.http")) {
            diag_.error(span,
                        "SecurityError: plugin '" + activePlugin_->name +
                            "' calls httpFetch() without 'net.http' permission");
        }
    }
}

void TypeChecker::checkPlugin(const PluginDecl& pl) {
    if (!pl.hasVersion || pl.version.empty()) {
        diag_.error(pl.span, "Plugin '" + pl.name + "' is missing required 'version' metadata");
    }
    if (!pl.hasPermissions) {
        diag_.error(pl.span, "Plugin '" + pl.name + "' is missing required 'permissions' list");
    }
    for (const auto& perm : pl.permissions) {
        bool valid = (perm == "fs.read" || perm == "fs.write" || perm == "net.http" ||
                      perm.rfind("process.exec:", 0) == 0);
        if (!valid) {
            diag_.error(pl.span,
                        "Plugin '" + pl.name + "' declares invalid permission '" + perm +
                            "' (allowed: fs.read, fs.write, net.http, process.exec:<cmd>)");
        }
    }

    tplugNamespaces_.insert(pl.name);

    // Register plugin structs (exported ones also visible globally under StructName)
    for (const auto& sd : pl.structs) {
        StructSig sig{sd.name, {}};
        for (const auto& f : sd.fields) sig.fields[f.name] = resolveTypeRef(f.type);
        structs_[sd.name] = sig;
    }

    // Register plugin function signatures
    activePluginLocalFns_.clear();
    for (const auto& fn : pl.functions) {
        FnSig sig;
        bool sawDefault = false;
        for (const auto& p : fn.params) {
            sig.params.push_back(resolveTypeRef(p.type));
            if (p.defaultValue) sawDefault = true;
            else {
                if (sawDefault) diag_.error(p.span, "Required parameter '" + p.name + "' cannot follow a default parameter");
                sig.minArgs++;
            }
        }
        sig.returnType = fn.returnType ? resolveTypeRef(*fn.returnType) : Type::voidT();
        activePluginLocalFns_[fn.name] = sig;
        if (fn.isExported) {
            tplugExportedFns_[pl.name + "." + fn.name] = sig;
        } else {
            tplugPrivateFns_[pl.name].insert(fn.name);
        }
    }

    // Register exported plugin components under "<plugin>.<Component>"
    for (const auto& comp : pl.components) {
        if (comp.isExported) {
            components_[pl.name + "." + comp.name] = &comp;
        }
    }

    // Type-check plugin bodies & lifecycle hooks under capability enforcement
    activePlugin_ = &pl;
    pushScope();
    for (const auto& fn : pl.functions) {
        checkFn(fn);
    }
    if (!pl.onLoad.empty()) {
        pushScope();
        for (const auto& s : pl.onLoad) checkStmt(*s);
        popScope();
    }
    if (!pl.onUnload.empty()) {
        pushScope();
        for (const auto& s : pl.onUnload) checkStmt(*s);
        popScope();
    }
    for (const auto& comp : pl.components) {
        pushScope();
        for (const auto& p : comp.params) {
            Type pt = resolveTypeRef(p.type);
            if (p.defaultValue) {
                Type dt = checkExpr(*p.defaultValue);
                if (!isAssignable(pt, dt)) {
                    diag_.error(p.span, "Default value for component param '" + p.name + "' expects " + pt.name() + ", got " + dt.name());
                }
            }
            defineVar(p.name, pt);
        }
        inComponent_ = true;
        if (comp.view) checkViewNode(*comp.view);
        inComponent_ = false;
        popScope();
    }
    popScope();
    activePlugin_ = nullptr;
    activePluginLocalFns_.clear();
}

void TypeChecker::checkWindowProps(const std::vector<PropArg>& props) {
    static const std::unordered_map<std::string, TypeKind> kWinProps = {
        {"width", TypeKind::Int},
        {"height", TypeKind::Int},
        {"bg", TypeKind::String},
    };
    for (const auto& p : props) {
        if (p.name.empty()) {
            diag_.error(p.span, "App window properties must be named (width, height, bg)");
            continue;
        }
        auto it = kWinProps.find(p.name);
        if (it == kWinProps.end()) {
            diag_.error(p.span, "Unknown app window property '" + p.name + "'");
            continue;
        }
        Type valType = checkExpr(*p.value);
        Type expected = Type::primitive(it->second);
        if (!isAssignable(expected, valType)) {
            diag_.error(p.span,
                        "App property '" + p.name + "' expects " +
                            expected.name() + ", got " + valType.name());
        }
    }
}

void TypeChecker::checkFn(const FnDecl& fn) {
    pushScope();
    Type prevRet = currentReturnType_;
    const FnSig* sigPtr = nullptr;
    if (activePlugin_) {
        auto pit = activePluginLocalFns_.find(fn.name);
        if (pit != activePluginLocalFns_.end()) sigPtr = &pit->second;
    }
    if (!sigPtr) {
        auto it = functions_.find(fn.name);
        if (it != functions_.end()) sigPtr = &it->second;
    }
    currentReturnType_ = sigPtr
                             ? sigPtr->returnType
                             : (fn.returnType ? resolveTypeRef(*fn.returnType) : Type::voidT());
    for (size_t i = 0; i < fn.params.size(); ++i) {
        Type pt = (sigPtr && i < sigPtr->params.size())
                      ? sigPtr->params[i]
                      : resolveTypeRef(fn.params[i].type);
        if (fn.params[i].defaultValue) {
            Type dt = checkExpr(*fn.params[i].defaultValue);
            if (!isAssignable(pt, dt)) {
                diag_.error(fn.params[i].span,
                            "Default value for parameter '" + fn.params[i].name + "' expects " +
                                pt.name() + ", got " + dt.name());
            }
        }
        defineVar(fn.params[i].name, pt);
    }
    for (const auto& s : fn.body) {
        checkStmt(*s);
    }
    currentReturnType_ = prevRet;
    popScope();
}

void TypeChecker::checkStmt(const Stmt& stmt) {
    if (auto* letS = std::get_if<LetStmt>(&stmt.node)) {
        Type initType = checkExpr(*letS->init);
        Type finalType = initType;
        if (letS->typeAnnot) {
            Type annotType = resolveTypeRef(*letS->typeAnnot);
            if (!isAssignable(annotType, initType)) {
                diag_.error(stmt.span,
                            "Cannot assign " + initType.name() + " to '" +
                                letS->name + "' of type " + annotType.name());
            }
            finalType = annotType;
        }
        defineVar(letS->name, finalType);
    } else if (auto* assignS = std::get_if<AssignStmt>(&stmt.node)) {
        if (constVars_.count(assignS->target)) {
            diag_.error(stmt.span, "Cannot assign to constant '" + assignS->target + "'");
            return;
        }
        if (derivedVars_.count(assignS->target)) {
            diag_.error(stmt.span,
                        "Cannot assign to derived signal '" + assignS->target + "' (derived values are read-only)");
            return;
        }
        auto varType = lookupVar(assignS->target);
        if (!varType) {
            diag_.error(stmt.span, "Undefined variable '" + assignS->target + "'");
            return;
        }
        Type rhsType = checkExpr(*assignS->value);
        if (assignS->op != "=") {
            if (assignS->op == "+=" && varType->kind == TypeKind::String && rhsType.kind == TypeKind::String) {
                return;
            }
            if (!varType->isNumeric() || !rhsType.isNumeric()) {
                diag_.error(stmt.span,
                            "Operator '" + assignS->op + "' requires numeric operands");
                return;
            }
        }
        if (!isAssignable(*varType, rhsType)) {
            diag_.error(stmt.span,
                        "Cannot assign " + rhsType.name() + " to '" +
                            assignS->target + "' of type " + varType->name());
        }
    } else if (auto* idxS = std::get_if<IndexAssignStmt>(&stmt.node)) {
        Type objT = checkExpr(*idxS->object);
        Type idxT = checkExpr(*idxS->index);
        Type valT = checkExpr(*idxS->value);
        if (!objT.isError()) {
            if (objT.kind == TypeKind::List) {
                if (!idxT.isError() && idxT.kind != TypeKind::Int) {
                    diag_.error(idxS->index->span, "List index must be Int, got " + idxT.name());
                }
                if (objT.element && !isAssignable(*objT.element, valT)) {
                    diag_.error(stmt.span, "Cannot assign " + valT.name() + " to element of " + objT.name());
                }
            } else if (objT.kind == TypeKind::Map) {
                if (!idxT.isError() && idxT.kind != TypeKind::String) {
                    diag_.error(idxS->index->span, "Map key must be String, got " + idxT.name());
                }
                if (objT.element && !isAssignable(*objT.element, valT)) {
                    diag_.error(stmt.span, "Cannot assign " + valT.name() + " to value of " + objT.name());
                }
            } else {
                diag_.error(stmt.span, "Cannot index type " + objT.name() + " (expected List or Map)");
            }
        }
    } else if (auto* fldS = std::get_if<FieldAssignStmt>(&stmt.node)) {
        Type objT = checkExpr(*fldS->object);
        Type valT = checkExpr(*fldS->value);
        if (!objT.isError()) {
            if (objT.kind != TypeKind::Struct) {
                diag_.error(stmt.span, "Cannot access field '" + fldS->field + "' on non-struct " + objT.name());
            } else {
                const auto& sig = structs_[objT.structName];
                auto it = sig.fields.find(fldS->field);
                if (it == sig.fields.end()) {
                    diag_.error(stmt.span, "Struct '" + objT.structName + "' has no field '" + fldS->field + "'");
                } else if (!isAssignable(it->second, valT)) {
                    diag_.error(stmt.span,
                                "Field '" + fldS->field + "' expects " + it->second.name() +
                                    ", got " + valT.name());
                }
            }
        }
    } else if (auto* ifS = std::get_if<IfStmt>(&stmt.node)) {
        Type ct = checkExpr(*ifS->condition);
        if (ct.kind != TypeKind::Bool && !ct.isError()) {
            diag_.error(ifS->condition->span, "If condition must be Bool, got " + ct.name());
        }
        pushScope();
        for (const auto& s : ifS->thenBody) checkStmt(*s);
        popScope();
        if (!ifS->elseBody.empty()) {
            pushScope();
            for (const auto& s : ifS->elseBody) checkStmt(*s);
            popScope();
        }
    } else if (auto* matchS = std::get_if<MatchStmt>(&stmt.node)) {
        Type st = checkExpr(*matchS->scrutinee);
        if (st.kind == TypeKind::Enum) {
            std::string enumName = st.structName;
            std::vector<std::string> covered;
            for (const auto& arm : matchS->arms) {
                std::string variant;
                std::string patEnum = enumName;
                if (auto* mem = std::get_if<MemberExpr>(&arm.pattern->node)) {
                    if (auto* oid = std::get_if<IdentExpr>(&mem->object->node)) {
                        patEnum = oid->name;
                        variant = mem->field;
                    }
                } else if (auto* bid = std::get_if<IdentExpr>(&arm.pattern->node)) {
                    variant = bid->name;
                }
                auto eit = enums_.find(patEnum);
                if (variant.empty() || eit == enums_.end()) {
                    diag_.error(arm.span, "match arm must be an enum variant such as Enum.Variant");
                } else {
                    if (!enumName.empty() && patEnum != enumName) {
                        diag_.error(arm.span, "match arm belongs to enum '" + patEnum + "' but the value is '" + enumName + "'");
                    } else if (std::find(eit->second.begin(), eit->second.end(), variant) == eit->second.end()) {
                        diag_.error(arm.span, "Enum '" + patEnum + "' has no variant '" + variant + "'");
                    } else if (std::find(covered.begin(), covered.end(), variant) != covered.end()) {
                        diag_.error(arm.span, "Duplicate match arm for variant '" + variant + "'");
                    } else {
                        covered.push_back(variant);
                    }
                }
                pushScope();
                for (const auto& s : arm.body) checkStmt(*s);
                popScope();
            }
            if (matchS->hasElse) {
                pushScope();
                for (const auto& s : matchS->elseBody) checkStmt(*s);
                popScope();
            } else if (!enumName.empty() && enums_.count(enumName)) {
                std::string missing;
                for (const auto& v : enums_[enumName]) {
                    if (std::find(covered.begin(), covered.end(), v) == covered.end()) {
                        missing += (missing.empty() ? "" : ", ") + v;
                    }
                }
                if (!missing.empty()) {
                    diag_.error(stmt.span, "Non-exhaustive match on '" + enumName + "': missing " + missing +
                                               " (add the arms or an 'else' arm)");
                }
            }
        } else {
            // Primitive value match (Int, String, Bool): requires at least one arm + an else arm
            if (!st.isError() && (matchS->arms.empty() ||
                                  (st.kind != TypeKind::Int && st.kind != TypeKind::String && st.kind != TypeKind::Bool))) {
                diag_.error(matchS->scrutinee->span, "match requires an enum value (or Int/String/Bool with pattern arms), got " + st.name());
            }
            for (const auto& arm : matchS->arms) {
                Type pt = checkExpr(*arm.pattern);
                if (!st.isError() && !pt.isError() && !isAssignable(st, pt)) {
                    diag_.error(arm.span, "match arm pattern type " + pt.name() + " does not match " + st.name());
                }
                pushScope();
                for (const auto& s : arm.body) checkStmt(*s);
                popScope();
            }
            if (matchS->hasElse) {
                pushScope();
                for (const auto& s : matchS->elseBody) checkStmt(*s);
                popScope();
            } else if (!matchS->arms.empty()) {
                diag_.error(stmt.span, "Non-exhaustive match on " + st.name() + ": add an 'else' arm");
            }
        }
    } else if (auto* whileS = std::get_if<WhileStmt>(&stmt.node)) {
        Type ct = checkExpr(*whileS->condition);
        if (ct.kind != TypeKind::Bool && !ct.isError()) {
            diag_.error(whileS->condition->span, "While condition must be Bool, got " + ct.name());
        }
        pushScope();
        loopDepth_++;
        for (const auto& s : whileS->body) checkStmt(*s);
        loopDepth_--;
        popScope();
    } else if (auto* forS = std::get_if<ForStmt>(&stmt.node)) {
        Type it = checkExpr(*forS->iterable);
        Type elemT = Type::anyT();
        if (!it.isError()) {
            if (it.kind != TypeKind::List) {
                diag_.error(forS->iterable->span, "For loop requires a List, got " + it.name());
            } else if (it.element) {
                elemT = *it.element;
            }
        }
        pushScope();
        if (!forS->indexVar.empty()) defineVar(forS->indexVar, Type::intT());
        defineVar(forS->itemVar, elemT);
        loopDepth_++;
        for (const auto& s : forS->body) checkStmt(*s);
        loopDepth_--;
        popScope();
    } else if (std::holds_alternative<BreakStmt>(stmt.node)) {
        if (loopDepth_ <= 0) diag_.error(stmt.span, "'break' is only allowed inside a 'while' or 'for' loop");
    } else if (std::holds_alternative<ContinueStmt>(stmt.node)) {
        if (loopDepth_ <= 0) diag_.error(stmt.span, "'continue' is only allowed inside a 'while' or 'for' loop");
    } else if (auto* retS = std::get_if<ReturnStmt>(&stmt.node)) {
        if (!retS->value) {
            if (currentReturnType_.kind != TypeKind::Void && !currentReturnType_.isError()) {
                diag_.error(stmt.span, "Expected return value of type " + currentReturnType_.name());
            }
        } else {
            Type vt = checkExpr(*retS->value);
            if (!isAssignable(currentReturnType_, vt)) {
                if (currentReturnType_.kind == TypeKind::Void) {
                    diag_.error(stmt.span,
                                "Function has no return type annotation (defaults to Void), but returns " +
                                    vt.name() + " (add '-> " + vt.name() + "' after parameters)");
                } else {
                    diag_.error(stmt.span,
                                "Return type mismatch: expected " +
                                    currentReturnType_.name() + ", got " + vt.name());
                }
            }
        }
    } else if (auto* exprS = std::get_if<ExprStmt>(&stmt.node)) {
        checkExpr(*exprS->expr);
    }
}

Type TypeChecker::checkExpr(const Expr& expr) {
    if (std::holds_alternative<IntLitExpr>(expr.node)) return Type::intT();
    if (std::holds_alternative<FloatLitExpr>(expr.node)) return Type::floatT();
    if (std::holds_alternative<BoolLitExpr>(expr.node)) return Type::boolT();
    if (std::holds_alternative<StringLitExpr>(expr.node)) return Type::stringT();
    if (std::holds_alternative<NullLitExpr>(expr.node)) return Type::anyT();

    if (auto* interp = std::get_if<InterpExpr>(&expr.node)) {
        for (const auto& part : interp->parts) checkExpr(*part);
        return Type::stringT();
    }

    if (auto* listLit = std::get_if<ListLitExpr>(&expr.node)) {
        if (listLit->elements.empty()) return Type::listOf(Type::anyT());
        Type firstT = checkExpr(*listLit->elements[0]);
        for (size_t i = 1; i < listLit->elements.size(); ++i) {
            Type et = checkExpr(*listLit->elements[i]);
            if (!isAssignable(firstT, et) && !isAssignable(et, firstT)) {
                diag_.error(listLit->elements[i]->span,
                            "List element type mismatch: expected " + firstT.name() + ", got " + et.name());
            }
        }
        return Type::listOf(firstT);
    }

    if (auto* si = std::get_if<StructInitExpr>(&expr.node)) {
        auto it = structs_.find(si->typeName);
        if (it == structs_.end()) {
            for (const auto& [fname, fexpr] : si->fields) {
                if (fexpr) checkExpr(*fexpr);
            }
            if (functions_.count(si->typeName)) {
                diag_.error(expr.span,
                            "Function '" + si->typeName +
                                "' must be called with positional arguments (e.g. '" +
                                si->typeName + "(...)'), not named 'param: value' arguments");
                return functions_[si->typeName].returnType;
            }
            if (components_.count(si->typeName) || widgetRegistry().find(si->typeName)) {
                diag_.error(expr.span,
                            "UI component/widget '" + si->typeName +
                                "' can only be used inside a 'view' tree, not as a value expression inside 'fn'");
                return Type::error();
            }
            diag_.error(expr.span, "Unknown struct or function '" + si->typeName + "'");
            return Type::error();
        }
        const StructSig& sig = it->second;
        std::unordered_set<std::string> provided;
        for (const auto& [fname, fexpr] : si->fields) {
            Type vt = checkExpr(*fexpr);
            auto fit = sig.fields.find(fname);
            if (fit == sig.fields.end()) {
                diag_.error(fexpr->span, "Struct '" + si->typeName + "' has no field '" + fname + "'");
            } else if (!isAssignable(fit->second, vt)) {
                diag_.error(fexpr->span,
                            "Field '" + fname + "' on '" + si->typeName + "' expects " +
                                fit->second.name() + ", got " + vt.name());
            }
            provided.insert(fname);
        }
        for (const auto& [fname, ftype] : sig.fields) {
            if (!provided.count(fname)) {
                diag_.error(expr.span, "Missing required field '" + fname + "' in '" + si->typeName + "'");
            }
        }
        return Type::structOf(si->typeName);
    }

    if (auto* idx = std::get_if<IndexExpr>(&expr.node)) {
        Type objT = checkExpr(*idx->object);
        Type idxT = checkExpr(*idx->index);
        if (objT.isError()) return Type::error();
        if (objT.kind == TypeKind::List) {
            if (!idxT.isError() && idxT.kind != TypeKind::Int) {
                diag_.error(idx->index->span, "List index must be Int, got " + idxT.name());
            }
            return objT.element ? *objT.element : Type::anyT();
        }
        if (objT.kind == TypeKind::Map) {
            if (!idxT.isError() && idxT.kind != TypeKind::String) {
                diag_.error(idx->index->span, "Map key must be String, got " + idxT.name());
            }
            return objT.element ? *objT.element : Type::anyT();
        }
        diag_.error(expr.span, "Cannot index non-list/map type " + objT.name());
        return Type::error();
    }

    if (auto* mem = std::get_if<MemberExpr>(&expr.node)) {
        // Enum.Variant constant
        if (auto* eid = std::get_if<IdentExpr>(&mem->object->node)) {
            auto eit = enums_.find(eid->name);
            if (eit != enums_.end() && !lookupVar(eid->name)) {
                const auto& vs = eit->second;
                if (std::find(vs.begin(), vs.end(), mem->field) == vs.end()) {
                    std::string list;
                    for (const auto& v : vs) list += (list.empty() ? "" : ", ") + v;
                    diag_.error(expr.span, "Enum '" + eid->name + "' has no variant '" + mem->field +
                                               "' (variants: " + list + ")");
                    return Type::error();
                }
                return Type::enumOf(eid->name);
            }
        }
        Type objT = checkExpr(*mem->object);
        if (objT.isError()) return Type::error();
        if (objT.kind != TypeKind::Struct) {
            diag_.error(expr.span, "Cannot access field '" + mem->field + "' on non-struct " + objT.name());
            return Type::error();
        }
        const auto& sig = structs_[objT.structName];
        auto it = sig.fields.find(mem->field);
        if (it == sig.fields.end()) {
            diag_.error(expr.span, "Struct '" + objT.structName + "' has no field '" + mem->field + "'");
            return Type::error();
        }
        return it->second;
    }

    if (auto* id = std::get_if<IdentExpr>(&expr.node)) {
        auto t = lookupVar(id->name);
        if (!t) {
            diag_.error(expr.span, "Undefined identifier '" + id->name + "'");
            return Type::error();
        }
        if (activeDeps_ && (stateVars_.count(id->name) || derivedVars_.count(id->name))) {
            activeDeps_->insert(id->name);
        }
        return *t;
    }

    if (auto* un = std::get_if<UnaryExpr>(&expr.node)) {
        Type ot = checkExpr(*un->operand);
        if (ot.isError()) return Type::error();
        if (un->op == "-") {
            if (!ot.isNumeric()) {
                diag_.error(expr.span, "Unary '-' requires numeric operand, got " + ot.name());
                return Type::error();
            }
            return ot;
        }
        if (un->op == "!") {
            if (ot.kind != TypeKind::Bool) {
                diag_.error(expr.span, "Unary '!' requires Bool operand, got " + ot.name());
                return Type::error();
            }
            return Type::boolT();
        }
        return Type::error();
    }

    if (auto* bin = std::get_if<BinaryExpr>(&expr.node)) {
        Type lt = checkExpr(*bin->left);
        Type rt = checkExpr(*bin->right);
        if (lt.isError() || rt.isError()) return Type::error();

        const std::string& op = bin->op;
        if (op == "+" && lt.kind == TypeKind::String && rt.kind == TypeKind::String) {
            return Type::stringT();
        }
        if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
            if (!lt.isNumeric() || !rt.isNumeric()) {
                diag_.error(expr.span,
                            "Operator '" + op + "' cannot be applied to " +
                                lt.name() + " and " + rt.name());
                return Type::error();
            }
            if (op == "%" && (lt.kind != TypeKind::Int || rt.kind != TypeKind::Int)) {
                diag_.error(expr.span, "Operator '%' requires Int operands");
                return Type::error();
            }
            return (lt.kind == TypeKind::Float || rt.kind == TypeKind::Float)
                       ? Type::floatT()
                       : Type::intT();
        }
        if (op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (!lt.isNumeric() || !rt.isNumeric()) {
                diag_.error(expr.span, "Comparison '" + op + "' requires numeric operands");
                return Type::error();
            }
            return Type::boolT();
        }
        if (op == "==" || op == "!=") {
            if (!isAssignable(lt, rt) && !isAssignable(rt, lt)) {
                diag_.error(expr.span, "Cannot compare " + lt.name() + " with " + rt.name());
                return Type::error();
            }
            return Type::boolT();
        }
        if (op == "??") {
            if (lt.kind == TypeKind::Result && lt.element) {
                if (!isAssignable(*lt.element, rt) && !isAssignable(rt, *lt.element)) {
                    diag_.error(expr.span, "Operator '?\?' fallback type " + rt.name() + " does not match " + lt.name());
                    return Type::error();
                }
                return *lt.element;
            }
            if (!isAssignable(lt, rt) && !isAssignable(rt, lt)) {
                diag_.error(expr.span, "Operator '?\?' requires compatible types, got " + lt.name() + " and " + rt.name());
                return Type::error();
            }
            return rt;
        }
        if (op == "&&" || op == "||") {
            if (lt.kind != TypeKind::Bool || rt.kind != TypeKind::Bool) {
                diag_.error(expr.span, "Logical '" + op + "' requires Bool operands");
                return Type::error();
            }
            return Type::boolT();
        }
        return Type::error();
    }

    if (auto* me = std::get_if<MatchExpr>(&expr.node)) {
        Type st = checkExpr(*me->scrutinee);
        Type resultT = Type::anyT();
        for (const auto& arm : me->arms) {
            if (st.kind == TypeKind::Enum) {
                std::string variant;
                if (auto* mem = std::get_if<MemberExpr>(&arm.pattern->node)) variant = mem->field;
                else if (auto* bid = std::get_if<IdentExpr>(&arm.pattern->node)) variant = bid->name;
                auto eit = enums_.find(st.structName);
                if (eit != enums_.end() && std::find(eit->second.begin(), eit->second.end(), variant) == eit->second.end()) {
                    diag_.error(arm.span, "Enum '" + st.structName + "' has no variant '" + variant + "'");
                }
            } else {
                checkExpr(*arm.pattern);
            }
            Type vt = checkExpr(*arm.value);
            if (resultT.kind == TypeKind::Any) resultT = vt;
        }
        if (me->elseExpr) {
            Type et = checkExpr(*me->elseExpr);
            if (resultT.kind == TypeKind::Any) resultT = et;
        }
        return resultT;
    }

    if (auto* ife = std::get_if<IfExpr>(&expr.node)) {
        Type ct = checkExpr(*ife->condition);
        if (ct.kind != TypeKind::Bool && !ct.isError()) {
            diag_.error(ife->condition->span, "If expression condition must be Bool, got " + ct.name());
        }
        Type tt = checkExpr(*ife->thenExpr);
        Type et = checkExpr(*ife->elseExpr);
        if (tt.isError() || et.isError()) return Type::error();
        if (tt == et) return tt;
        if (tt.isNumeric() && et.isNumeric()) return Type::floatT();
        diag_.error(expr.span,
                    "If expression branches have incompatible types: " +
                        tt.name() + " and " + et.name());
        return Type::error();
    }

    if (auto* call = std::get_if<CallExpr>(&expr.node)) {
        checkCapabilityCall(call->callee, call->args, expr.span);

        const FnSig* sigPtr = nullptr;
        if (activePlugin_) {
            auto pit = activePluginLocalFns_.find(call->callee);
            if (pit != activePluginLocalFns_.end()) sigPtr = &pit->second;
        }
        if (!sigPtr) {
            auto it = functions_.find(call->callee);
            if (it != functions_.end()) sigPtr = &it->second;
        }
        if (!sigPtr) {
            for (const auto& a : call->args) {
                if (a) checkExpr(*a);
            }
            if (components_.count(call->callee) || widgetRegistry().find(call->callee)) {
                diag_.error(expr.span,
                            "UI component/widget '" + call->callee +
                                "' can only be used inside a 'view' tree, not as a value expression inside 'fn'");
                return Type::error();
            }
            diag_.error(expr.span, "Unknown function '" + call->callee + "'");
            return Type::error();
        }
        const FnSig& sig = *sigPtr;
        if (sig.specialBuiltin) {
            if (call->callee == "len") {
                if (call->args.size() != 1) {
                    diag_.error(expr.span, "len() expects 1 argument");
                    return Type::intT();
                }
                Type at = checkExpr(*call->args[0]);
                if (!at.isError() && at.kind != TypeKind::String && at.kind != TypeKind::List && at.kind != TypeKind::Map) {
                    diag_.error(call->args[0]->span, "len() expects String, List, or Map, got " + at.name());
                }
                return Type::intT();
            }
            if (call->callee == "push") {
                if (call->args.size() != 2) {
                    diag_.error(expr.span, "push(list, value) expects 2 arguments");
                    return Type::voidT();
                }
                Type lt = checkExpr(*call->args[0]);
                Type vt = checkExpr(*call->args[1]);
                if (!lt.isError()) {
                    if (lt.kind != TypeKind::List) {
                        diag_.error(call->args[0]->span, "push() first argument must be a List");
                    } else if (lt.element && !isAssignable(*lt.element, vt)) {
                        diag_.error(call->args[1]->span,
                                    "Cannot push " + vt.name() + " into " + lt.name());
                    }
                }
                return Type::voidT();
            }
            if (call->callee == "removeAt") {
                if (call->args.size() != 2) {
                    diag_.error(expr.span, "removeAt(list, index) expects 2 arguments");
                    return Type::voidT();
                }
                Type lt = checkExpr(*call->args[0]);
                Type it2 = checkExpr(*call->args[1]);
                if (!lt.isError() && lt.kind != TypeKind::List) {
                    diag_.error(call->args[0]->span, "removeAt() first argument must be a List");
                }
                if (!it2.isError() && it2.kind != TypeKind::Int) {
                    diag_.error(call->args[1]->span, "removeAt() index must be Int");
                }
                return Type::voidT();
            }
            if (call->callee == "clear") {
                if (call->args.size() != 1) {
                    diag_.error(expr.span, "clear(collection) expects 1 argument");
                    return Type::voidT();
                }
                Type lt = checkExpr(*call->args[0]);
                if (!lt.isError() && lt.kind != TypeKind::List && lt.kind != TypeKind::Map) {
                    diag_.error(call->args[0]->span, "clear() argument must be a List or Map");
                }
                return Type::voidT();
            }
            if (call->callee == "hasKey" || call->callee == "removeKey") {
                if (call->args.size() != 2) {
                    diag_.error(expr.span, call->callee + "(map, key) expects 2 arguments");
                    return call->callee == "hasKey" ? Type::boolT() : Type::voidT();
                }
                Type mt = checkExpr(*call->args[0]);
                Type kt = checkExpr(*call->args[1]);
                if (!mt.isError() && mt.kind != TypeKind::Map) diag_.error(call->args[0]->span, call->callee + "() expects a Map");
                if (!kt.isError() && kt.kind != TypeKind::String) diag_.error(call->args[1]->span, call->callee + "() key must be String");
                return call->callee == "hasKey" ? Type::boolT() : Type::voidT();
            }
            if (call->callee == "keys") {
                if (call->args.size() != 1) {
                    diag_.error(expr.span, "keys(map) expects 1 argument");
                    return Type::listOf(Type::stringT());
                }
                Type mt = checkExpr(*call->args[0]);
                if (!mt.isError() && mt.kind != TypeKind::Map) diag_.error(call->args[0]->span, "keys() expects a Map");
                return Type::listOf(Type::stringT());
            }
            if (call->callee == "Ok") {
                if (call->args.size() != 1) {
                    diag_.error(expr.span, "Ok(value) expects 1 argument");
                    return Type::resultOf(Type::anyT());
                }
                return Type::resultOf(checkExpr(*call->args[0]));
            }
            if (call->callee == "isOk" || call->callee == "isErr" || call->callee == "errMsg" || call->callee == "unwrap") {
                if (call->args.size() != 1) {
                    diag_.error(expr.span, call->callee + "(result) expects 1 argument");
                    return Type::anyT();
                }
                Type rt = checkExpr(*call->args[0]);
                if (!rt.isError() && rt.kind != TypeKind::Result) {
                    diag_.error(call->args[0]->span, call->callee + "() expects a Result<T>");
                }
                if (call->callee == "isOk" || call->callee == "isErr") return Type::boolT();
                if (call->callee == "errMsg") return Type::stringT();
                return (rt.kind == TypeKind::Result && rt.element) ? *rt.element : Type::anyT();
            }
            if (call->callee == "unwrapOr") {
                if (call->args.size() != 2) {
                    diag_.error(expr.span, "unwrapOr(result, fallback) expects 2 arguments");
                    return Type::anyT();
                }
                Type rt = checkExpr(*call->args[0]);
                Type ft = checkExpr(*call->args[1]);
                if (!rt.isError() && rt.kind != TypeKind::Result) {
                    diag_.error(call->args[0]->span, "unwrapOr() first argument must be Result<T>");
                }
                return ft;
            }
            if (call->callee == "listJoin") {
                if (call->args.size() != 2) {
                    diag_.error(expr.span, "listJoin(list, sep) expects 2 arguments");
                    return Type::stringT();
                }
                Type lt = checkExpr(*call->args[0]);
                Type st = checkExpr(*call->args[1]);
                if (!lt.isError() && lt.kind != TypeKind::List) diag_.error(call->args[0]->span, "listJoin() expects a List");
                if (!st.isError() && st.kind != TypeKind::String) diag_.error(call->args[1]->span, "listJoin() separator must be String");
                return Type::stringT();
            }
            if (call->callee == "listReverse" || call->callee == "listSort") {
                if (call->args.size() != 1) {
                    diag_.error(expr.span, call->callee + "(list) expects 1 argument");
                    return Type::listOf(Type::anyT());
                }
                Type lt = checkExpr(*call->args[0]);
                if (!lt.isError() && lt.kind != TypeKind::List) {
                    diag_.error(call->args[0]->span, call->callee + "() expects a List");
                    return Type::listOf(Type::anyT());
                }
                return lt;
            }
            if (call->callee == "listContains") {
                if (call->args.size() != 2) {
                    diag_.error(expr.span, "listContains(list, item) expects 2 arguments");
                    return Type::boolT();
                }
                Type lt = checkExpr(*call->args[0]);
                Type vt = checkExpr(*call->args[1]);
                if (!lt.isError() && lt.kind != TypeKind::List) {
                    diag_.error(call->args[0]->span, "listContains() first argument must be a List");
                } else if (lt.element && !isAssignable(*lt.element, vt)) {
                    diag_.error(call->args[1]->span, "listContains() item type " + vt.name() + " does not match " + lt.name());
                }
                return Type::boolT();
            }
            if (call->callee == "listSlice") {
                if (call->args.size() != 3) {
                    diag_.error(expr.span, "listSlice(list, start, end) expects 3 arguments");
                    return Type::listOf(Type::anyT());
                }
                Type lt = checkExpr(*call->args[0]);
                Type a = checkExpr(*call->args[1]);
                Type b = checkExpr(*call->args[2]);
                if (!lt.isError() && lt.kind != TypeKind::List) diag_.error(call->args[0]->span, "listSlice() expects a List");
                if (!a.isError() && a.kind != TypeKind::Int) diag_.error(call->args[1]->span, "listSlice() start must be Int");
                if (!b.isError() && b.kind != TypeKind::Int) diag_.error(call->args[2]->span, "listSlice() end must be Int");
                return lt.kind == TypeKind::List ? lt : Type::listOf(Type::anyT());
            }
        }

        if (call->args.size() < sig.minArgs || call->args.size() > sig.params.size()) {
            std::string expectedStr = (sig.minArgs == sig.params.size())
                                          ? std::to_string(sig.params.size())
                                          : (std::to_string(sig.minArgs) + ".." + std::to_string(sig.params.size()));
            diag_.error(expr.span,
                        "Function '" + call->callee + "' expects " + expectedStr +
                            " argument(s), got " + std::to_string(call->args.size()));
            return sig.returnType;
        }
        for (size_t i = 0; i < call->args.size(); ++i) {
            Type at = checkExpr(*call->args[i]);
            if (!isAssignable(sig.params[i], at)) {
                diag_.error(call->args[i]->span,
                            "Argument " + std::to_string(i + 1) + " of '" +
                                call->callee + "' expects " + sig.params[i].name() +
                                ", got " + at.name());
            }
        }
        return sig.returnType;
    }

    if (auto* mcall = std::get_if<MethodCallExpr>(&expr.node)) {
        std::string qname = mcall->object + "." + mcall->method;

        // 1. Check .tplug official language modules first
        if (tplugNamespaces_.count(mcall->object)) {
            auto it = tplugExportedFns_.find(qname);
            if (it == tplugExportedFns_.end()) {
                if (tplugPrivateFns_[mcall->object].count(mcall->method)) {
                    diag_.error(expr.span,
                                "Function '" + mcall->method + "' in plugin '" + mcall->object +
                                    "' is private (mark it with 'export fn' to expose it)");
                } else {
                    diag_.error(expr.span,
                                "Plugin '" + mcall->object + "' has no exported function '" + mcall->method + "'");
                }
                return Type::error();
            }
            const FnSig& sig = it->second;
            if (mcall->args.size() < sig.minArgs || mcall->args.size() > sig.params.size()) {
                std::string expectedStr = (sig.minArgs == sig.params.size())
                                              ? std::to_string(sig.params.size())
                                              : (std::to_string(sig.minArgs) + ".." + std::to_string(sig.params.size()));
                diag_.error(expr.span,
                            "Plugin function '" + qname + "' expects " + expectedStr +
                                " argument(s), got " + std::to_string(mcall->args.size()));
                return sig.returnType;
            }
            for (size_t i = 0; i < mcall->args.size(); ++i) {
                Type at = checkExpr(*mcall->args[i]);
                if (!isAssignable(sig.params[i], at)) {
                    diag_.error(mcall->args[i]->span,
                                "Argument " + std::to_string(i + 1) + " of '" + qname +
                                    "' expects " + sig.params[i].name() + ", got " + at.name());
                }
            }
            return sig.returnType;
        }

        // 2. Check if 'object' is actually a local/state variable (user tried calling a method on a value)
        if (auto varT = lookupVar(mcall->object)) {
            for (const auto& a : mcall->args) {
                if (a) checkExpr(*a);
            }
            if (mcall->method == "toString") {
                diag_.error(expr.span,
                            "Method '.toString()' is not supported on values; use 'toString(" +
                                mcall->object + ")' or string interpolation \"{" + mcall->object + "}\"");
                return Type::stringT();
            }
            if (mcall->method == "len" || mcall->method == "length" || mcall->method == "size") {
                diag_.error(expr.span,
                            "Method '." + mcall->method + "()' is not supported on values; use 'len(" +
                                mcall->object + ")'");
                return Type::intT();
            }
            if (mcall->method == "push" || mcall->method == "append") {
                diag_.error(expr.span,
                            "Method '." + mcall->method + "()' is not supported; use 'push(" +
                                mcall->object + ", value)'");
                return Type::voidT();
            }
            diag_.error(expr.span,
                        "Cannot call method '." + mcall->method + "()' on variable '" +
                            mcall->object + "' of type " + varT->name() +
                            " (use standard library functions instead)");
            return Type::error();
        }

        // 3. Fall back to native C-ABI .dylib plugins
        if (!plugins_ || !plugins_->hasNamespace(mcall->object)) {
            for (const auto& a : mcall->args) {
                if (a) checkExpr(*a);
            }
            diag_.error(expr.span,
                        "Unknown plugin namespace '" + mcall->object +
                            "' (did you forget `use \"" + mcall->object + "\"`?)");
            return Type::error();
        }
        const PluginFnInfo* info = plugins_->findFunction(qname);
        if (!info) {
            diag_.error(expr.span, "Plugin '" + mcall->object + "' has no function '" + mcall->method + "'");
            return Type::error();
        }
        if (mcall->args.size() != info->params.size()) {
            diag_.error(expr.span,
                        "Plugin function '" + qname + "' expects " +
                            std::to_string(info->params.size()) + " argument(s), got " +
                            std::to_string(mcall->args.size()));
            return info->returnType;
        }
        for (size_t i = 0; i < mcall->args.size(); ++i) {
            Type at = checkExpr(*mcall->args[i]);
            if (!isAssignable(info->params[i], at)) {
                diag_.error(mcall->args[i]->span,
                            "Argument " + std::to_string(i + 1) + " of '" + qname +
                                "' expects " + info->params[i].name() + ", got " + at.name());
            }
        }
        return info->returnType;
    }

    return Type::error();
}

void TypeChecker::checkViewNode(const ViewNode& node) {
    if (node.kind == ViewNodeKind::Slot) {
        if (!inComponent_) {
            diag_.error(node.span, "'slot' can only be used inside a 'component' definition");
        }
        return;
    }

    if (node.kind == ViewNodeKind::If) {
        Type ct = checkExpr(*node.condition);
        if (!ct.isError() && ct.kind != TypeKind::Bool) {
            diag_.error(node.condition->span, "If view condition must be Bool, got " + ct.name());
        }
        for (const auto& c : node.children) checkViewNode(*c);
        for (const auto& c : node.elseChildren) checkViewNode(*c);
        return;
    }

    if (node.kind == ViewNodeKind::For) {
        Type iterT = checkExpr(*node.iterable);
        Type elemT = Type::anyT();
        if (!iterT.isError()) {
            if (iterT.kind != TypeKind::List) {
                diag_.error(node.iterable->span, "For view requires a List, got " + iterT.name());
            } else if (iterT.element) {
                elemT = *iterT.element;
            }
        }
        pushScope();
        if (!node.indexVar.empty()) defineVar(node.indexVar, Type::intT());
        defineVar(node.itemVar, elemT);
        for (const auto& c : node.children) checkViewNode(*c);
        popScope();
        return;
    }

    // Custom component
    auto compIt = components_.find(node.widget);
    if (compIt != components_.end()) {
        const ComponentDecl* comp = compIt->second;
        for (const auto& prop : node.props) {
            Type pt = checkExpr(*prop.value);
            if (!prop.name.empty()) {
                bool found = false;
                for (const auto& param : comp->params) {
                    if (param.name == prop.name) {
                        found = true;
                        Type expected = resolveTypeRef(param.type);
                        if (!isAssignable(expected, pt)) {
                            diag_.error(prop.span,
                                        "Component '" + comp->name + "' prop '" +
                                            prop.name + "' expects " + expected.name() +
                                            ", got " + pt.name());
                        }
                        break;
                    }
                }
                if (!found) {
                    diag_.error(prop.span,
                                "Component '" + comp->name + "' has no parameter '" + prop.name + "'");
                }
            }
        }
        for (const auto& child : node.children) {
            checkViewNode(*child);
        }
        return;
    }

    const WidgetSchemaRegistry& widgets = widgetRegistry();
    const WidgetSpec* specPtr = widgets.find(node.widget);
    if (!specPtr) {
        diag_.error(node.span, "Unknown widget or component '" + node.widget + "'");
        return;
    }

    const WidgetSpec& spec = *specPtr;
    for (const auto& prop : node.props) {
        std::string propName = prop.name.empty() ? spec.positionalProp : prop.name;
        if (propName.empty()) {
            diag_.error(prop.span,
                        "Widget '" + node.widget + "' does not accept positional arguments");
            continue;
        }
        if (propName == "class") {
            Type vt = checkExpr(*prop.value);
            if (!isAssignable(Type::stringT(), vt)) {
                diag_.error(prop.span, "Property 'class' expects String, got " + vt.name());
                continue;
            }
            if (auto* lit = std::get_if<StringLitExpr>(&prop.value->node)) {
                std::istringstream iss(lit->value);
                std::string cname;
                while (iss >> cname) {
                    if (!styleSheet_ || !styleSheet_->classes.count(cname)) {
                        diag_.error(prop.span, "Unknown style class '" + cname + "'");
                    }
                }
            }
            continue;
        }
        if (propName == "borderColor" || propName == "borderWidth") {
            Type vt = checkExpr(*prop.value);
            Type expected = (propName == "borderColor") ? Type::stringT() : Type::intT();
            if (!isAssignable(expected, vt)) {
                diag_.error(prop.span, "Property '" + propName + "' expects " + expected.name() + ", got " + vt.name());
            }
            continue;
        }
        auto pit = spec.props.find(propName);
        if (pit == spec.props.end()) {
            diag_.error(prop.span,
                        "Widget '" + node.widget + "' has no property '" + propName + "'");
            continue;
        }
        if (pit->second.requiresStateIdent) {
            auto* ident = std::get_if<IdentExpr>(&prop.value->node);
            if (!ident || !stateVars_.count(ident->name)) {
                diag_.error(prop.span,
                            "Property '" + propName + "' on '" + node.widget +
                                "' must bind directly to a mutable 'state' variable");
                continue;
            }
        }
        Type valType = checkExpr(*prop.value);
        Type expected = (pit->second.type == TypeKind::List)
                            ? Type::listOf(Type::anyT())
                            : Type::primitive(pit->second.type);
        if (!isAssignable(expected, valType)) {
            diag_.error(prop.span,
                        "Property '" + propName + "' on '" + node.widget +
                            "' expects " + expected.name() + ", got " + valType.name());
        }
    }

    if (!node.children.empty() && !spec.allowsChildren) {
        diag_.error(node.span, "Widget '" + node.widget + "' cannot have child views");
    }
    for (const auto& child : node.children) {
        checkViewNode(*child);
    }

    if (!node.action.empty()) {
        if (!spec.allowsAction) {
            diag_.error(node.span, "Widget '" + node.widget + "' does not accept an action block");
        } else {
            pushScope();
            for (const auto& s : node.action) checkStmt(*s);
            popScope();
        }
    }
}

} // namespace transom
