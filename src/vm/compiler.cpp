#include "vm/compiler.hpp"

#include <algorithm>

namespace transom {

int Compiler::lookupBuiltin(const std::string& name) {
    static const std::unordered_map<std::string, int> kBuiltins = {
        {"abs", 0},
        {"round", 1},
        {"min", 2},
        {"max", 3},
        {"clamp", 4},
        {"len", 5},
        {"toString", 6},
        {"toInt", 7},
        {"toFloat", 8},
        {"print", 9},
        {"push", 10},
        {"removeAt", 11},
        {"clear", 12},
        {"fsRead", 13},
        {"fsWrite", 14},
        {"fsExists", 15},
        {"storageGet", 16},
        {"storageSet", 17},
        {"clipboardCopy", 18},
        {"clipboardPaste", 19},
        {"openUrl", 20},
        {"dialogOpenFile", 21},
        {"dialogSaveFile", 22},
        {"httpFetch", 23},
        {"jsonGet", 24},
        {"bytesRead", 25},
        {"bytesLen", 26},
        {"bytesAt", 27},
        {"bytesSlice", 28},
        {"bytesToString", 29},
        {"bytesToHex", 30},
        {"strTrim", 31},
        {"strSplit", 32},
        {"strContains", 33},
        {"strStartsWith", 34},
        {"strReplace", 35},
        {"strCharAt", 36},
        {"strSub", 37},
        {"sysExec", 38},
        {"setTheme", 39},
        {"httpRequest", 40},
        {"jsonPath", 41},
        {"mapNew", 42},
        {"hasKey", 43},
        {"keys", 44},
        {"removeKey", 45},
        {"range", 46},
        {"Ok", 47},
        {"Err", 48},
        {"isOk", 49},
        {"isErr", 50},
        {"unwrap", 51},
        {"unwrapOr", 52},
        {"errMsg", 53},
        {"__coalesce", 54},
        {"eventName", 55},
        {"eventPayload", 56},
        {"listJoin", 57},
        {"listReverse", 58},
        {"listSort", 59},
        {"listContains", 60},
        {"listSlice", 61},
        {"strUpper", 62},
        {"strLower", 63},
    };
    auto it = kBuiltins.find(name);
    return (it != kBuiltins.end()) ? it->second : -1;
}

int Compiler::resolveLocal(const std::string& name) const {
    for (int i = static_cast<int>(locals_.size()) - 1; i >= 0; --i) {
        if (locals_[i].name == name) return i;
    }
    return -1;
}

void Compiler::endScope(Chunk& chunk) {
    scopeDepth_--;
    while (!locals_.empty() && locals_.back().depth > scopeDepth_) {
        chunk.emit(OpCode::Pop);
        locals_.pop_back();
    }
}

CompiledProgram Compiler::compile(const Program& program) {
    CompiledProgram out;
    fnIndex_.clear();
    fnDecls_.clear();
    tplugFnIndex_.clear();
    tplugFnDecls_.clear();
    activePluginName_.clear();
    components_.clear();
    signals_.clear();
    enumNames_.clear();
    consts_.clear();
    for (const auto& ed : program.enums) enumNames_.insert(ed.name);
    for (const auto& cd : program.consts) consts_[cd.name] = cd.value.get();

    for (const auto& u : program.uses) {
        out.plugins.push_back(u.plugin);
    }
    out.styleSheet = program.styleSheet;

    for (const auto& comp : program.components) {
        components_[comp.name] = &comp;
    }

    // Register .tplug exported components & permissions
    for (const auto& pl : program.tplugs) {
        out.pluginPermissions[pl.name] = pl.permissions;
        for (const auto& comp : pl.components) {
            if (comp.isExported) {
                components_[pl.name + "." + comp.name] = &comp;
            }
        }
    }

    uint16_t idx = 0;
    for (const auto& pl : program.tplugs) {
        for (const auto& fn : pl.functions) {
            uint16_t fidx = idx++;
            fnIndex_[pl.name + "::" + fn.name] = fidx;
            fnDecls_[pl.name + "::" + fn.name] = &fn;
            if (fn.isExported) {
                tplugFnIndex_[pl.name + "." + fn.name] = fidx;
                tplugFnDecls_[pl.name + "." + fn.name] = &fn;
            }
        }
    }
    for (const auto& fn : program.functions) {
        fnIndex_[fn.name] = idx++;
        fnDecls_[fn.name] = &fn;
    }
    if (program.app) {
        for (const auto& fn : program.app->functions) {
            fnIndex_[fn.name] = idx++;
            fnDecls_[fn.name] = &fn;
        }
        for (const auto& st : program.app->states) {
            signals_.insert(st.name);
        }
        for (const auto& dr : program.app->deriveds) {
            signals_.insert(dr.name);
        }
    }

    // Compile .tplug functions & lifecycle hooks with ownerPlugin isolation
    for (const auto& pl : program.tplugs) {
        activePluginName_ = pl.name;
        for (const auto& fn : pl.functions) {
            auto cf = compileFn(fn);
            cf.ownerPlugin = pl.name;
            cf.chunk.name = "plugin " + pl.name + "." + fn.name;
            out.functions.push_back(std::move(cf));
        }
        if (!pl.onLoad.empty()) {
            out.pluginOnLoad.emplace_back(
                pl.name, compileActionChunk("plugin." + pl.name + ".onLoad", pl.onLoad));
        }
        if (!pl.onUnload.empty()) {
            out.pluginOnUnload.emplace_back(
                pl.name, compileActionChunk("plugin." + pl.name + ".onUnload", pl.onUnload));
        }
        activePluginName_.clear();
    }

    for (const auto& fn : program.functions) {
        out.functions.push_back(compileFn(fn));
    }

    if (program.app) {
        const AppDecl& app = *program.app;
        out.hasApp = true;
        out.app.title = app.title;

        for (const auto& fn : app.functions) {
            out.functions.push_back(compileFn(fn));
        }

        for (const auto& wp : app.windowProps) {
            CompiledProp cp;
            cp.name = wp.name;
            cp.expr = compileExprChunk("window." + wp.name, *wp.value);
            out.app.windowProps.push_back(std::move(cp));
        }

        for (const auto& st : app.states) {
            CompiledState cs;
            cs.name = st.name;
            cs.init = compileExprChunk("state." + st.name, *st.init);
            cs.isPersistent = st.isPersistent;
            out.app.states.push_back(std::move(cs));
        }

        for (const auto& dr : app.deriveds) {
            CompiledDerived cd;
            cd.name = dr.name;
            cd.expr = compileExprChunk("derived." + dr.name, *dr.expr);
            out.app.deriveds.push_back(std::move(cd));
        }

        int timerIdx = 0;
        for (const auto& tm : app.timers) {
            CompiledTimer ct;
            ct.intervalMs = compileExprChunk("timer.interval#" + std::to_string(++timerIdx), *tm.intervalMs);
            ct.body = compileActionChunk("timer.body#" + std::to_string(timerIdx), tm.body);
            out.app.timers.push_back(std::move(ct));
        }

        if (app.view) {
            int actionCounter = 0;
            out.app.rootView = compileView(*app.view, actionCounter);
        }
    }

    return out;
}

CompiledFn Compiler::compileFn(const FnDecl& fn) {
    CompiledFn cf;
    cf.name = fn.name;
    cf.arity = static_cast<uint8_t>(fn.params.size());
    cf.chunk.name = "fn " + fn.name;

    locals_.clear();
    scopeDepth_ = 1;
    for (const auto& p : fn.params) {
        locals_.push_back({p.name, scopeDepth_});
    }
    for (const auto& stmt : fn.body) {
        emitStmt(cf.chunk, *stmt);
    }
    cf.chunk.emit(OpCode::Null);
    cf.chunk.emit(OpCode::Return);
    locals_.clear();
    scopeDepth_ = 0;
    return cf;
}

CompiledExpr Compiler::compileExprChunk(const std::string& name, const Expr& expr) {
    CompiledExpr ce;
    ce.chunk.name = name;
    locals_.clear();
    scopeDepth_ = 0;

    std::unordered_set<std::string> deps;
    activeDeps_ = &deps;
    emitExpr(ce.chunk, expr);
    activeDeps_ = nullptr;

    ce.chunk.emit(OpCode::Return);
    ce.signalDeps.assign(deps.begin(), deps.end());
    std::sort(ce.signalDeps.begin(), ce.signalDeps.end());
    return ce;
}

Chunk Compiler::compileActionChunk(const std::string& name, const std::vector<StmtPtr>& stmts) {
    Chunk chunk;
    chunk.name = name;
    locals_.clear();
    scopeDepth_ = 1;
    for (const auto& s : stmts) {
        emitStmt(chunk, *s);
    }
    chunk.emit(OpCode::Null);
    chunk.emit(OpCode::Return);
    locals_.clear();
    scopeDepth_ = 0;
    return chunk;
}

void Compiler::appendCompiledChildren(const std::vector<ViewNodePtr>& nodes,
                                      std::vector<CompiledView>& out,
                                      int& actionCounter) {
    for (const auto& child : nodes) {
        if (child->kind == ViewNodeKind::Slot) {
            if (activeSlotChildren_) {
                auto savedBindings = componentBindings_;
                auto savedSlotChildren = activeSlotChildren_;
                auto savedSlotBindings = activeSlotBindings_;
                componentBindings_ = savedSlotBindings;
                activeSlotChildren_ = nullptr;
                for (const auto& sc : *savedSlotChildren) {
                    out.push_back(compileView(*sc, actionCounter));
                }
                componentBindings_ = std::move(savedBindings);
                activeSlotChildren_ = savedSlotChildren;
                activeSlotBindings_ = std::move(savedSlotBindings);
            }
        } else {
            out.push_back(compileView(*child, actionCounter));
        }
    }
}

CompiledView Compiler::compileView(const ViewNode& node, int& actionCounter) {
    if (node.kind == ViewNodeKind::Slot) {
        // Standalone slot as root of component: compile first slot child or wrap in VStack
        if (activeSlotChildren_ && !activeSlotChildren_->empty()) {
            auto savedBindings = componentBindings_;
            auto savedSlotChildren = activeSlotChildren_;
            auto savedSlotBindings = activeSlotBindings_;
            componentBindings_ = savedSlotBindings;
            activeSlotChildren_ = nullptr;
            CompiledView res;
            if (savedSlotChildren->size() == 1) {
                res = compileView(*(*savedSlotChildren)[0], actionCounter);
            } else {
                res.kind = CompiledViewKind::Widget;
                res.widget = "VStack";
                for (const auto& sc : *savedSlotChildren) {
                    res.children.push_back(compileView(*sc, actionCounter));
                }
            }
            componentBindings_ = std::move(savedBindings);
            activeSlotChildren_ = savedSlotChildren;
            activeSlotBindings_ = std::move(savedSlotBindings);
            return res;
        }
        CompiledView empty;
        empty.kind = CompiledViewKind::Widget;
        empty.widget = "VStack";
        return empty;
    }

    if (node.kind == ViewNodeKind::If) {
        CompiledView cv;
        cv.kind = CompiledViewKind::If;
        cv.condition = compileExprChunk("view.if", *node.condition);
        appendCompiledChildren(node.children, cv.children, actionCounter);
        appendCompiledChildren(node.elseChildren, cv.elseChildren, actionCounter);
        return cv;
    }

    if (node.kind == ViewNodeKind::For) {
        CompiledView cv;
        cv.kind = CompiledViewKind::For;
        cv.indexVar = node.indexVar;
        cv.itemVar = node.itemVar;
        cv.iterable = compileExprChunk("view.for", *node.iterable);
        appendCompiledChildren(node.children, cv.children, actionCounter);
        return cv;
    }

    // Expand user-defined component inline with prop bindings, default values, and slot children
    auto compIt = components_.find(node.widget);
    if (compIt != components_.end() && compIt->second->view) {
        const ComponentDecl* comp = compIt->second;
        auto savedBindings = componentBindings_;
        auto savedSlotChildren = activeSlotChildren_;
        auto savedSlotBindings = activeSlotBindings_;

        std::unordered_map<std::string, const Expr*> nextBindings;
        for (size_t i = 0; i < node.props.size(); ++i) {
            const auto& prop = node.props[i];
            std::string pname = prop.name;
            if (pname.empty() && i < comp->params.size()) {
                pname = comp->params[i].name;
            }
            if (!pname.empty()) {
                nextBindings[pname] = prop.value.get();
            }
        }
        for (const auto& param : comp->params) {
            if (!nextBindings.count(param.name) && param.defaultValue) {
                nextBindings[param.name] = param.defaultValue.get();
            }
        }
        componentBindings_ = std::move(nextBindings);
        activeSlotBindings_ = savedBindings;
        activeSlotChildren_ = &node.children;

        CompiledView expanded = compileView(*comp->view, actionCounter);

        componentBindings_ = std::move(savedBindings);
        activeSlotChildren_ = savedSlotChildren;
        activeSlotBindings_ = std::move(savedSlotBindings);
        return expanded;
    }

    CompiledView cv;
    cv.kind = CompiledViewKind::Widget;
    cv.widget = node.widget;

    auto positionalName = [&]() -> std::string {
        const WidgetSchemaRegistry& reg = widgets_ ? *widgets_ : WidgetSchemaRegistry::builtins();
        const WidgetSpec* spec = reg.find(node.widget);
        return spec ? spec->positionalProp : std::string();
    };

    for (const auto& prop : node.props) {
        CompiledProp cp;
        cp.name = prop.name.empty() ? positionalName() : prop.name;
        if (cp.name == "bind") {
            if (auto* id = std::get_if<IdentExpr>(&prop.value->node)) {
                cp.bindState = id->name;
            }
        }
        cp.expr = compileExprChunk(node.widget + "." + cp.name, *prop.value);
        cv.props.push_back(std::move(cp));
    }

    appendCompiledChildren(node.children, cv.children, actionCounter);

    if (!node.action.empty()) {
        cv.hasAction = true;
        cv.actionChunk = compileActionChunk(
            node.widget + ".onClick#" + std::to_string(++actionCounter),
            node.action);
    }

    return cv;
}

void Compiler::emitStmt(Chunk& chunk, const Stmt& stmt) {
    if (auto* letS = std::get_if<LetStmt>(&stmt.node)) {
        emitExpr(chunk, *letS->init);
        locals_.push_back({letS->name, scopeDepth_});
    } else if (auto* assignS = std::get_if<AssignStmt>(&stmt.node)) {
        int slot = resolveLocal(assignS->target);
        bool isSig = (slot < 0 && signals_.count(assignS->target) > 0);

        if (assignS->op != "=") {
            if (slot >= 0) {
                chunk.emit(OpCode::GetLocal);
                chunk.emitU16(static_cast<uint16_t>(slot));
            } else if (isSig) {
                uint16_t c = chunk.addConstant(assignS->target);
                chunk.emit(OpCode::GetSignal);
                chunk.emitU16(c);
            }
            emitExpr(chunk, *assignS->value);
            if (assignS->op == "+=") chunk.emit(OpCode::Add);
            else if (assignS->op == "-=") chunk.emit(OpCode::Sub);
            else if (assignS->op == "*=") chunk.emit(OpCode::Mul);
            else if (assignS->op == "/=") chunk.emit(OpCode::Div);
        } else {
            emitExpr(chunk, *assignS->value);
        }

        if (slot >= 0) {
            chunk.emit(OpCode::SetLocal);
            chunk.emitU16(static_cast<uint16_t>(slot));
        } else if (isSig) {
            uint16_t c = chunk.addConstant(assignS->target);
            chunk.emit(OpCode::SetSignal);
            chunk.emitU16(c);
        }
        chunk.emit(OpCode::Pop);
    } else if (auto* idxS = std::get_if<IndexAssignStmt>(&stmt.node)) {
        emitExpr(chunk, *idxS->object);
        emitExpr(chunk, *idxS->index);
        emitExpr(chunk, *idxS->value);
        chunk.emit(OpCode::SetIndex);
        if (!idxS->rootVar.empty() && signals_.count(idxS->rootVar)) {
            uint16_t c = chunk.addConstant(Value(idxS->rootVar));
            chunk.emit(OpCode::NotifySignal);
            chunk.emitU16(c);
        }
    } else if (auto* fldS = std::get_if<FieldAssignStmt>(&stmt.node)) {
        emitExpr(chunk, *fldS->object);
        emitExpr(chunk, *fldS->value);
        uint16_t fc = chunk.addConstant(Value(fldS->field));
        chunk.emit(OpCode::SetField);
        chunk.emitU16(fc);
        if (!fldS->rootVar.empty() && signals_.count(fldS->rootVar)) {
            uint16_t c = chunk.addConstant(Value(fldS->rootVar));
            chunk.emit(OpCode::NotifySignal);
            chunk.emitU16(c);
        }
    } else if (auto* ifS = std::get_if<IfStmt>(&stmt.node)) {
        emitExpr(chunk, *ifS->condition);
        size_t elseJump = chunk.emitJump(OpCode::JumpIfFalse);
        chunk.emit(OpCode::Pop);
        beginScope();
        for (const auto& s : ifS->thenBody) emitStmt(chunk, *s);
        endScope(chunk);
        size_t endJump = chunk.emitJump(OpCode::Jump);
        chunk.patchJump(elseJump);
        chunk.emit(OpCode::Pop);
        beginScope();
        for (const auto& s : ifS->elseBody) emitStmt(chunk, *s);
        endScope(chunk);
        chunk.patchJump(endJump);
    } else if (auto* matchS = std::get_if<MatchStmt>(&stmt.node)) {
        // Lowered to a hidden local + comparison chain (no new VM opcodes).
        beginScope();
        emitExpr(chunk, *matchS->scrutinee);
        std::string hidden = "$match" + std::to_string(matchCounter_++);
        locals_.push_back({hidden, scopeDepth_});
        int slot = resolveLocal(hidden);
        std::vector<size_t> endJumps;
        for (const auto& arm : matchS->arms) {
            chunk.emit(OpCode::GetLocal);
            chunk.emitU16(static_cast<uint16_t>(slot));
            if (auto* pm = std::get_if<MemberExpr>(&arm.pattern->node)) {
                chunk.emit(OpCode::Const);
                chunk.emitU16(chunk.addConstant(Value(pm->field)));
            } else if (auto* pid = std::get_if<IdentExpr>(&arm.pattern->node)) {
                chunk.emit(OpCode::Const);
                chunk.emitU16(chunk.addConstant(Value(pid->name)));
            } else {
                emitExpr(chunk, *arm.pattern);
            }
            chunk.emit(OpCode::Eq);
            size_t next = chunk.emitJump(OpCode::JumpIfFalse);
            chunk.emit(OpCode::Pop);
            beginScope();
            for (const auto& s : arm.body) emitStmt(chunk, *s);
            endScope(chunk);
            endJumps.push_back(chunk.emitJump(OpCode::Jump));
            chunk.patchJump(next);
            chunk.emit(OpCode::Pop);
        }
        if (matchS->hasElse) {
            beginScope();
            for (const auto& s : matchS->elseBody) emitStmt(chunk, *s);
            endScope(chunk);
        }
        for (size_t j : endJumps) chunk.patchJump(j);
        endScope(chunk);
    } else if (auto* whileS = std::get_if<WhileStmt>(&stmt.node)) {
        uint16_t loopStart = static_cast<uint16_t>(chunk.code.size());
        emitExpr(chunk, *whileS->condition);
        size_t exitJump = chunk.emitJump(OpCode::JumpIfFalse);
        chunk.emit(OpCode::Pop);
        beginScope();
        loopStack_.push_back(LoopContext{scopeDepth_, false, -1, loopStart, {}, {}});
        for (const auto& s : whileS->body) emitStmt(chunk, *s);
        auto ctx = std::move(loopStack_.back());
        loopStack_.pop_back();
        endScope(chunk);
        for (size_t cj : ctx.continueJumps) {
            chunk.code[cj] = static_cast<uint8_t>((loopStart >> 8) & 0xFF);
            chunk.code[cj + 1] = static_cast<uint8_t>(loopStart & 0xFF);
        }
        chunk.emit(OpCode::Jump);
        chunk.emitU16(loopStart);
        chunk.patchJump(exitJump);
        chunk.emit(OpCode::Pop);
        for (size_t bj : ctx.breakJumps) chunk.patchJump(bj);
    } else if (auto* forS = std::get_if<ForStmt>(&stmt.node)) {
        beginScope();
        int loopId = matchCounter_++;
        // $list = <iterable>
        emitExpr(chunk, *forS->iterable);
        std::string listName = "$forList" + std::to_string(loopId);
        locals_.push_back({listName, scopeDepth_});
        int listSlot = resolveLocal(listName);
        // $idx = 0
        chunk.emit(OpCode::Const);
        chunk.emitU16(chunk.addConstant(Value(static_cast<int64_t>(0))));
        std::string idxName = "$forIdx" + std::to_string(loopId);
        locals_.push_back({idxName, scopeDepth_});
        int idxSlot = resolveLocal(idxName);

        uint16_t loopStart = static_cast<uint16_t>(chunk.code.size());
        // condition: $idx < len($list)
        chunk.emit(OpCode::GetLocal);
        chunk.emitU16(static_cast<uint16_t>(idxSlot));
        chunk.emit(OpCode::GetLocal);
        chunk.emitU16(static_cast<uint16_t>(listSlot));
        chunk.emit(OpCode::CallBuiltin);
        chunk.emitU16(5); // len
        chunk.emitU8(1);
        chunk.emit(OpCode::Lt);
        size_t exitJump = chunk.emitJump(OpCode::JumpIfFalse);
        chunk.emit(OpCode::Pop);

        beginScope();
        if (!forS->indexVar.empty()) {
            chunk.emit(OpCode::GetLocal);
            chunk.emitU16(static_cast<uint16_t>(idxSlot));
            locals_.push_back({forS->indexVar, scopeDepth_});
        }
        // itemVar = $list[$idx]
        chunk.emit(OpCode::GetLocal);
        chunk.emitU16(static_cast<uint16_t>(listSlot));
        chunk.emit(OpCode::GetLocal);
        chunk.emitU16(static_cast<uint16_t>(idxSlot));
        chunk.emit(OpCode::GetIndex);
        locals_.push_back({forS->itemVar, scopeDepth_});

        loopStack_.push_back(LoopContext{scopeDepth_, true, idxSlot, 0, {}, {}});
        for (const auto& s : forS->body) emitStmt(chunk, *s);
        auto ctx = std::move(loopStack_.back());
        loopStack_.pop_back();
        endScope(chunk);

        // step target for continue: $idx = $idx + 1
        for (size_t cj : ctx.continueJumps) chunk.patchJump(cj);
        chunk.emit(OpCode::GetLocal);
        chunk.emitU16(static_cast<uint16_t>(idxSlot));
        chunk.emit(OpCode::Const);
        chunk.emitU16(chunk.addConstant(Value(static_cast<int64_t>(1))));
        chunk.emit(OpCode::Add);
        chunk.emit(OpCode::SetLocal);
        chunk.emitU16(static_cast<uint16_t>(idxSlot));
        chunk.emit(OpCode::Pop);

        chunk.emit(OpCode::Jump);
        chunk.emitU16(loopStart);
        chunk.patchJump(exitJump);
        chunk.emit(OpCode::Pop);
        for (size_t bj : ctx.breakJumps) chunk.patchJump(bj);
        endScope(chunk);
    } else if (std::holds_alternative<BreakStmt>(stmt.node)) {
        if (!loopStack_.empty()) {
            auto& ctx = loopStack_.back();
            for (int i = static_cast<int>(locals_.size()) - 1; i >= 0 && locals_[i].depth >= ctx.scopeDepth; --i) {
                chunk.emit(OpCode::Pop);
            }
            ctx.breakJumps.push_back(chunk.emitJump(OpCode::Jump));
        }
    } else if (std::holds_alternative<ContinueStmt>(stmt.node)) {
        if (!loopStack_.empty()) {
            auto& ctx = loopStack_.back();
            for (int i = static_cast<int>(locals_.size()) - 1; i >= 0 && locals_[i].depth >= ctx.scopeDepth; --i) {
                chunk.emit(OpCode::Pop);
            }
            ctx.continueJumps.push_back(chunk.emitJump(OpCode::Jump));
        }
    } else if (auto* retS = std::get_if<ReturnStmt>(&stmt.node)) {
        if (retS->value) {
            emitExpr(chunk, *retS->value);
        } else {
            chunk.emit(OpCode::Null);
        }
        chunk.emit(OpCode::Return);
    } else if (auto* exprS = std::get_if<ExprStmt>(&stmt.node)) {
        emitExpr(chunk, *exprS->expr);
        chunk.emit(OpCode::Pop);
    }
}

void Compiler::emitExpr(Chunk& chunk, const Expr& expr) {
    if (auto* i = std::get_if<IntLitExpr>(&expr.node)) {
        uint16_t c = chunk.addConstant(Value(i->value));
        chunk.emit(OpCode::Const);
        chunk.emitU16(c);
    } else if (auto* f = std::get_if<FloatLitExpr>(&expr.node)) {
        uint16_t c = chunk.addConstant(Value(f->value));
        chunk.emit(OpCode::Const);
        chunk.emitU16(c);
    } else if (auto* b = std::get_if<BoolLitExpr>(&expr.node)) {
        chunk.emit(b->value ? OpCode::True : OpCode::False);
    } else if (auto* s = std::get_if<StringLitExpr>(&expr.node)) {
        uint16_t c = chunk.addConstant(Value(s->value));
        chunk.emit(OpCode::Const);
        chunk.emitU16(c);
    } else if (std::holds_alternative<NullLitExpr>(expr.node)) {
        chunk.emit(OpCode::Null);
    } else if (auto* interp = std::get_if<InterpExpr>(&expr.node)) {
        if (interp->parts.empty()) {
            uint16_t c = chunk.addConstant(Value(""));
            chunk.emit(OpCode::Const);
            chunk.emitU16(c);
            return;
        }
        for (const auto& part : interp->parts) {
            emitExpr(chunk, *part);
            chunk.emit(OpCode::ToString);
        }
        chunk.emit(OpCode::Concat);
        chunk.emitU16(static_cast<uint16_t>(interp->parts.size()));
    } else if (auto* listLit = std::get_if<ListLitExpr>(&expr.node)) {
        for (const auto& el : listLit->elements) {
            emitExpr(chunk, *el);
        }
        chunk.emit(OpCode::MakeList);
        chunk.emitU16(static_cast<uint16_t>(listLit->elements.size()));
    } else if (auto* si = std::get_if<StructInitExpr>(&expr.node)) {
        for (const auto& [fname, fexpr] : si->fields) {
            uint16_t fnConst = chunk.addConstant(Value(fname));
            chunk.emit(OpCode::Const);
            chunk.emitU16(fnConst);
            emitExpr(chunk, *fexpr);
        }
        uint16_t typeConst = chunk.addConstant(Value(si->typeName));
        chunk.emit(OpCode::MakeStruct);
        chunk.emitU16(typeConst);
        chunk.emitU16(static_cast<uint16_t>(si->fields.size()));
    } else if (auto* idx = std::get_if<IndexExpr>(&expr.node)) {
        emitExpr(chunk, *idx->object);
        emitExpr(chunk, *idx->index);
        chunk.emit(OpCode::GetIndex);
    } else if (auto* mem = std::get_if<MemberExpr>(&expr.node)) {
        if (auto* eid = std::get_if<IdentExpr>(&mem->object->node)) {
            if (enumNames_.count(eid->name) && resolveLocal(eid->name) < 0 && !signals_.count(eid->name)) {
                // Enum.Variant compiles to its variant-name string constant.
                chunk.emit(OpCode::Const);
                chunk.emitU16(chunk.addConstant(Value(mem->field)));
                return;
            }
        }
        emitExpr(chunk, *mem->object);
        uint16_t fc = chunk.addConstant(Value(mem->field));
        chunk.emit(OpCode::GetField);
        chunk.emitU16(fc);
    } else if (auto* id = std::get_if<IdentExpr>(&expr.node)) {
        auto bit = componentBindings_.find(id->name);
        if (bit != componentBindings_.end()) {
            const Expr* boundExpr = bit->second;
            auto saved = std::move(componentBindings_);
            componentBindings_.clear();
            emitExpr(chunk, *boundExpr);
            componentBindings_ = std::move(saved);
            return;
        }
        int slot = resolveLocal(id->name);
        if (slot >= 0) {
            chunk.emit(OpCode::GetLocal);
            chunk.emitU16(static_cast<uint16_t>(slot));
        } else if (consts_.count(id->name)) {
            emitExpr(chunk, *consts_.at(id->name));
        } else {
            if (activeDeps_ && signals_.count(id->name)) {
                activeDeps_->insert(id->name);
            }
            uint16_t c = chunk.addConstant(Value(id->name));
            chunk.emit(OpCode::GetSignal);
            chunk.emitU16(c);
        }
    } else if (auto* un = std::get_if<UnaryExpr>(&expr.node)) {
        emitExpr(chunk, *un->operand);
        if (un->op == "-") chunk.emit(OpCode::Neg);
        else if (un->op == "!") chunk.emit(OpCode::Not);
    } else if (auto* bin = std::get_if<BinaryExpr>(&expr.node)) {
        if (bin->op == "??") {
            emitExpr(chunk, *bin->left);
            emitExpr(chunk, *bin->right);
            chunk.emit(OpCode::CallBuiltin);
            chunk.emitU16(54); // __coalesce
            chunk.emitU8(2);
            return;
        }
        if (bin->op == "&&") {
            emitExpr(chunk, *bin->left);
            size_t endJump = chunk.emitJump(OpCode::JumpIfFalse);
            chunk.emit(OpCode::Pop);
            emitExpr(chunk, *bin->right);
            chunk.patchJump(endJump);
            return;
        }
        if (bin->op == "||") {
            emitExpr(chunk, *bin->left);
            size_t tryRight = chunk.emitJump(OpCode::JumpIfFalse);
            size_t endJump = chunk.emitJump(OpCode::Jump);
            chunk.patchJump(tryRight);
            chunk.emit(OpCode::Pop);
            emitExpr(chunk, *bin->right);
            chunk.patchJump(endJump);
            return;
        }
        emitExpr(chunk, *bin->left);
        emitExpr(chunk, *bin->right);
        if (bin->op == "+") chunk.emit(OpCode::Add);
        else if (bin->op == "-") chunk.emit(OpCode::Sub);
        else if (bin->op == "*") chunk.emit(OpCode::Mul);
        else if (bin->op == "/") chunk.emit(OpCode::Div);
        else if (bin->op == "%") chunk.emit(OpCode::Mod);
        else if (bin->op == "==") chunk.emit(OpCode::Eq);
        else if (bin->op == "!=") chunk.emit(OpCode::Neq);
        else if (bin->op == "<") chunk.emit(OpCode::Lt);
        else if (bin->op == "<=") chunk.emit(OpCode::Le);
        else if (bin->op == ">") chunk.emit(OpCode::Gt);
        else if (bin->op == ">=") chunk.emit(OpCode::Ge);
    } else if (auto* me = std::get_if<MatchExpr>(&expr.node)) {
        // Recursive conditional chain: leaves the matched arm value on top of the stack
        std::vector<size_t> endJumps;
        for (const auto& arm : me->arms) {
            emitExpr(chunk, *me->scrutinee);
            if (auto* pm = std::get_if<MemberExpr>(&arm.pattern->node)) {
                chunk.emit(OpCode::Const);
                chunk.emitU16(chunk.addConstant(Value(pm->field)));
            } else if (auto* pid = std::get_if<IdentExpr>(&arm.pattern->node)) {
                chunk.emit(OpCode::Const);
                chunk.emitU16(chunk.addConstant(Value(pid->name)));
            } else {
                emitExpr(chunk, *arm.pattern);
            }
            chunk.emit(OpCode::Eq);
            size_t nextArm = chunk.emitJump(OpCode::JumpIfFalse);
            chunk.emit(OpCode::Pop);
            emitExpr(chunk, *arm.value);
            endJumps.push_back(chunk.emitJump(OpCode::Jump));
            chunk.patchJump(nextArm);
            chunk.emit(OpCode::Pop);
        }
        if (me->elseExpr) {
            emitExpr(chunk, *me->elseExpr);
        } else {
            chunk.emit(OpCode::Null);
        }
        for (size_t j : endJumps) chunk.patchJump(j);
    } else if (auto* ife = std::get_if<IfExpr>(&expr.node)) {
        emitExpr(chunk, *ife->condition);
        size_t elseJump = chunk.emitJump(OpCode::JumpIfFalse);
        chunk.emit(OpCode::Pop);
        emitExpr(chunk, *ife->thenExpr);
        size_t endJump = chunk.emitJump(OpCode::Jump);
        chunk.patchJump(elseJump);
        chunk.emit(OpCode::Pop);
        emitExpr(chunk, *ife->elseExpr);
        chunk.patchJump(endJump);
    } else if (auto* call = std::get_if<CallExpr>(&expr.node)) {
        for (const auto& arg : call->args) {
            emitExpr(chunk, *arg);
        }
        int builtin = lookupBuiltin(call->callee);
        if (builtin >= 0) {
            chunk.emit(OpCode::CallBuiltin);
            chunk.emitU16(static_cast<uint16_t>(builtin));
            chunk.emitU8(static_cast<uint8_t>(call->args.size()));

            // Auto-notify reactive signal when mutating a list state in place
            if ((call->callee == "push" || call->callee == "removeAt" || call->callee == "clear" || call->callee == "removeKey") &&
                !call->args.empty()) {
                if (auto* id = std::get_if<IdentExpr>(&call->args[0]->node)) {
                    if (signals_.count(id->name)) {
                        uint16_t c = chunk.addConstant(Value(id->name));
                        chunk.emit(OpCode::NotifySignal);
                        chunk.emitU16(c);
                    }
                }
            }
        } else {
            uint16_t fidx = 0;
            const FnDecl* targetDecl = nullptr;
            if (!activePluginName_.empty()) {
                std::string pkey = activePluginName_ + "::" + call->callee;
                auto pit = fnIndex_.find(pkey);
                if (pit != fnIndex_.end()) {
                    fidx = pit->second;
                    auto dit = fnDecls_.find(pkey);
                    if (dit != fnDecls_.end()) targetDecl = dit->second;
                } else {
                    auto fit = fnIndex_.find(call->callee);
                    if (fit != fnIndex_.end()) fidx = fit->second;
                    auto dit = fnDecls_.find(call->callee);
                    if (dit != fnDecls_.end()) targetDecl = dit->second;
                }
            } else {
                auto fit = fnIndex_.find(call->callee);
                if (fit != fnIndex_.end()) fidx = fit->second;
                auto dit = fnDecls_.find(call->callee);
                if (dit != fnDecls_.end()) targetDecl = dit->second;
            }
            size_t totalArgs = call->args.size();
            if (targetDecl && totalArgs < targetDecl->params.size()) {
                for (size_t i = totalArgs; i < targetDecl->params.size(); ++i) {
                    if (targetDecl->params[i].defaultValue) {
                        emitExpr(chunk, *targetDecl->params[i].defaultValue);
                    } else {
                        chunk.emit(OpCode::Null);
                    }
                    totalArgs++;
                }
            }
            chunk.emit(OpCode::Call);
            chunk.emitU16(fidx);
            chunk.emitU8(static_cast<uint8_t>(totalArgs));
        }
    } else if (auto* mcall = std::get_if<MethodCallExpr>(&expr.node)) {
        for (const auto& arg : mcall->args) {
            emitExpr(chunk, *arg);
        }
        std::string qname = mcall->object + "." + mcall->method;
        auto tplugIt = tplugFnIndex_.find(qname);
        if (tplugIt != tplugFnIndex_.end()) {
            size_t totalArgs = mcall->args.size();
            auto dit = tplugFnDecls_.find(qname);
            if (dit != tplugFnDecls_.end() && dit->second && totalArgs < dit->second->params.size()) {
                for (size_t i = totalArgs; i < dit->second->params.size(); ++i) {
                    if (dit->second->params[i].defaultValue) {
                        emitExpr(chunk, *dit->second->params[i].defaultValue);
                    } else {
                        chunk.emit(OpCode::Null);
                    }
                    totalArgs++;
                }
            }
            chunk.emit(OpCode::Call);
            chunk.emitU16(tplugIt->second);
            chunk.emitU8(static_cast<uint8_t>(totalArgs));
        } else {
            uint16_t nameIdx = chunk.addConstant(Value(qname));
            chunk.emit(OpCode::CallPlugin);
            chunk.emitU16(nameIdx);
            chunk.emitU8(static_cast<uint8_t>(mcall->args.size()));
        }
    }
}

} // namespace transom
