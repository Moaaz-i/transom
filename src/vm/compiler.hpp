#pragma once

#include "frontend/ast.hpp"
#include "sema/widget_schema.hpp"
#include "vm/bytecode.hpp"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace transom {

class Compiler {
public:
    CompiledProgram compile(const Program& program);

    // Widget schemas (positional props etc). Defaults to core built-ins only.
    void setWidgetRegistry(const WidgetSchemaRegistry* reg) { widgets_ = reg; }

    static int lookupBuiltin(const std::string& name);

private:
    struct Local {
        std::string name;
        int depth = 0;
    };

    std::unordered_map<std::string, uint16_t> fnIndex_;
    std::unordered_map<std::string, const FnDecl*> fnDecls_;
    std::unordered_map<std::string, uint16_t> tplugFnIndex_; // "plugin.fn" -> fnIdx
    std::unordered_map<std::string, const FnDecl*> tplugFnDecls_;
    std::string activePluginName_;
    const WidgetSchemaRegistry* widgets_ = nullptr;
    std::unordered_map<std::string, const ComponentDecl*> components_;
    std::unordered_set<std::string> signals_;
    std::unordered_set<std::string> enumNames_;
    std::unordered_map<std::string, const Expr*> consts_;
    int matchCounter_ = 0;

    struct LoopContext {
        int scopeDepth = 0;
        bool isForLoop = false;
        int idxSlot = -1;
        uint16_t continueTarget = 0;
        std::vector<size_t> breakJumps;
        std::vector<size_t> continueJumps;
    };
    std::vector<LoopContext> loopStack_;

    // Current compilation state
    std::vector<Local> locals_;
    int scopeDepth_ = 0;
    std::unordered_set<std::string>* activeDeps_ = nullptr;

    // Optional parameter substitution map & slot children when expanding custom components
    std::unordered_map<std::string, const Expr*> componentBindings_;
    const std::vector<ViewNodePtr>* activeSlotChildren_ = nullptr;
    std::unordered_map<std::string, const Expr*> activeSlotBindings_;

    void appendCompiledChildren(const std::vector<ViewNodePtr>& nodes,
                                std::vector<CompiledView>& out,
                                int& actionCounter);

    int resolveLocal(const std::string& name) const;
    void beginScope() { scopeDepth_++; }
    void endScope(Chunk& chunk);

    CompiledFn compileFn(const FnDecl& fn);
    CompiledExpr compileExprChunk(const std::string& name, const Expr& expr);
    Chunk compileActionChunk(const std::string& name, const std::vector<StmtPtr>& stmts);
    CompiledView compileView(const ViewNode& node, int& actionCounter);

    void emitStmt(Chunk& chunk, const Stmt& stmt);
    void emitExpr(Chunk& chunk, const Expr& expr);
};

} // namespace transom
