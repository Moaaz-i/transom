#pragma once

#include "sema/widget_schema.hpp"

#include "common/source.hpp"
#include "frontend/ast.hpp"
#include "plugin/plugin_manager.hpp"
#include "sema/types.hpp"
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace transom {

struct FnSig {
    std::vector<Type> params;
    Type returnType = Type::voidT();
    bool specialBuiltin = false; // for polymorphic list builtins (push, removeAt, clear, len)
    size_t minArgs = 0;          // required args (params without default values)
};

struct StructSig {
    std::string name;
    std::unordered_map<std::string, Type> fields;
};

struct CheckedProgram {
    std::unordered_map<std::string, Type> stateTypes;
    std::unordered_map<std::string, Type> derivedTypes;
    std::unordered_map<std::string, std::unordered_set<std::string>> derivedDeps;
};

class TypeChecker {
public:
    explicit TypeChecker(DiagnosticEngine& diag, const PluginManager* plugins = nullptr);

    CheckedProgram check(const Program& program);

private:
    DiagnosticEngine& diag_;
    const PluginManager* plugins_ = nullptr;
    const WidgetSchemaRegistry& widgetRegistry() const {
        return plugins_ ? plugins_->widgets() : WidgetSchemaRegistry::builtins();
    }
    std::unordered_map<std::string, FnSig> functions_;
    std::unordered_map<std::string, StructSig> structs_;
    std::unordered_map<std::string, std::vector<std::string>> enums_;
    std::unordered_map<std::string, const ComponentDecl*> components_;

    // .tplug exported symbols (namespaced as "plugin.symbol")
    std::unordered_set<std::string> tplugNamespaces_;
    std::unordered_map<std::string, FnSig> tplugExportedFns_;
    std::unordered_map<std::string, std::unordered_set<std::string>> tplugPrivateFns_;
    const PluginDecl* activePlugin_ = nullptr;
    std::unordered_map<std::string, FnSig> activePluginLocalFns_;
    const CompiledStyleSheet* styleSheet_ = nullptr;

    std::unordered_set<std::string> stateVars_;
    std::unordered_set<std::string> derivedVars_;
    std::unordered_set<std::string> constVars_;
    int loopDepth_ = 0;
    bool inComponent_ = false;

    std::vector<std::unordered_map<std::string, Type>> scopes_;
    Type currentReturnType_ = Type::voidT();

    std::unordered_set<std::string>* activeDeps_ = nullptr;

    void registerBuiltins();
    void checkPlugin(const PluginDecl& pl);
    bool pluginHasPerm(const PluginDecl& pl, const std::string& perm) const;
    void checkCapabilityCall(const std::string& callee,
                             const std::vector<ExprPtr>& args,
                             Span span);
    void pushScope() { scopes_.emplace_back(); }
    void popScope() { scopes_.pop_back(); }
    void defineVar(const std::string& name, Type type) { scopes_.back()[name] = std::move(type); }
    std::optional<Type> lookupVar(const std::string& name) const;

    Type resolveTypeRef(const TypeRef& ref);
    void checkFn(const FnDecl& fn);
    void checkStmt(const Stmt& stmt);
    Type checkExpr(const Expr& expr);
    void checkViewNode(const ViewNode& node);
    void checkWindowProps(const std::vector<PropArg>& props);
};

} // namespace transom
