#pragma once

#include "common/source.hpp"
#include "style/stylesheet.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace transom {

// ---------------------------------------------------------------------------
// Type annotations in source (e.g. Int, String, List<Task>)
// ---------------------------------------------------------------------------
struct TypeRef {
    std::string name;
    std::vector<TypeRef> typeArgs;
    Span span;
};

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------
struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct IntLitExpr { int64_t value; };
struct FloatLitExpr { double value; };
struct BoolLitExpr { bool value; };
struct StringLitExpr { std::string value; };
struct NullLitExpr {};

struct InterpExpr {
    std::vector<ExprPtr> parts;
};

struct IdentExpr { std::string name; };

struct ListLitExpr {
    std::vector<ExprPtr> elements;
};

struct StructInitExpr {
    std::string typeName;
    std::vector<std::pair<std::string, ExprPtr>> fields;
};

struct IndexExpr {
    ExprPtr object;
    ExprPtr index;
};

struct MemberExpr {
    ExprPtr object;
    std::string field;
};

struct UnaryExpr {
    std::string op;
    ExprPtr operand;
};

struct BinaryExpr {
    std::string op;
    ExprPtr left;
    ExprPtr right;
};

struct CallExpr {
    std::string callee;
    std::vector<ExprPtr> args;
};

struct MethodCallExpr {
    std::string object;
    std::string method;
    std::vector<ExprPtr> args;
};

struct IfExpr {
    ExprPtr condition;
    ExprPtr thenExpr;
    ExprPtr elseExpr;
};

struct MatchExprArm {
    ExprPtr pattern;
    ExprPtr value;
    Span span;
};

struct MatchExpr {
    ExprPtr scrutinee;
    std::vector<MatchExprArm> arms;
    ExprPtr elseExpr;
};

struct Expr {
    Span span;
    std::variant<
        IntLitExpr,
        FloatLitExpr,
        BoolLitExpr,
        StringLitExpr,
        NullLitExpr,
        InterpExpr,
        IdentExpr,
        ListLitExpr,
        StructInitExpr,
        IndexExpr,
        MemberExpr,
        UnaryExpr,
        BinaryExpr,
        CallExpr,
        MethodCallExpr,
        IfExpr,
        MatchExpr
    > node;
};

// ---------------------------------------------------------------------------
// Statements (for fn bodies and UI event handlers)
// ---------------------------------------------------------------------------
struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct LetStmt {
    std::string name;
    std::optional<TypeRef> typeAnnot;
    ExprPtr init;
};

struct AssignStmt {
    std::string target;
    std::string op; // "=", "+=", "-=", "*=", "/="
    ExprPtr value;
};

struct IndexAssignStmt {
    ExprPtr object;
    std::string rootVar; // leftmost identifier for reactive signal notification
    ExprPtr index;
    ExprPtr value;
};

struct FieldAssignStmt {
    ExprPtr object;
    std::string rootVar; // leftmost identifier for reactive signal notification
    std::string field;
    ExprPtr value;
};

struct IfStmt {
    ExprPtr condition;
    std::vector<StmtPtr> thenBody;
    std::vector<StmtPtr> elseBody;
};

struct WhileStmt {
    ExprPtr condition;
    std::vector<StmtPtr> body;
};

struct ForStmt {
    std::string indexVar; // optional
    std::string itemVar;
    ExprPtr iterable;
    std::vector<StmtPtr> body;
};

struct BreakStmt {};
struct ContinueStmt {};

struct ReturnStmt {
    ExprPtr value;
};

struct ExprStmt {
    ExprPtr expr;
};

// match <enum value> { Enum.Variant { ... }  Variant { ... }  else { ... } }
struct MatchArm {
    ExprPtr pattern; // `Enum.Variant` or bare `Variant`
    std::vector<StmtPtr> body;
    Span span;
};

struct MatchStmt {
    ExprPtr scrutinee;
    std::vector<MatchArm> arms;
    bool hasElse = false;
    std::vector<StmtPtr> elseBody;
};

struct Stmt {
    Span span;
    std::variant<
        LetStmt,
        AssignStmt,
        IndexAssignStmt,
        FieldAssignStmt,
        IfStmt,
        WhileStmt,
        ForStmt,
        BreakStmt,
        ContinueStmt,
        ReturnStmt,
        ExprStmt,
        MatchStmt
    > node;
};

// ---------------------------------------------------------------------------
// Declarative UI Tree (Widget, IfView, ForView)
// ---------------------------------------------------------------------------
struct PropArg {
    std::string name; // empty if positional
    ExprPtr value;
    Span span;
};

enum class ViewNodeKind {
    Widget,
    If,
    For,
    Slot
};

struct ViewNode {
    ViewNodeKind kind = ViewNodeKind::Widget;

    // Widget fields
    std::string widget;
    std::vector<PropArg> props;
    std::vector<std::unique_ptr<ViewNode>> children;
    std::vector<StmtPtr> action;

    // IfView fields
    ExprPtr condition;
    std::vector<std::unique_ptr<ViewNode>> elseChildren;

    // ForView fields
    std::string indexVar; // optional (empty if `for item in list`)
    std::string itemVar;
    ExprPtr iterable;

    Span span;
};

using ViewNodePtr = std::unique_ptr<ViewNode>;

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------
struct Param {
    std::string name;
    TypeRef type;
    ExprPtr defaultValue; // optional default expression (e.g. `radius: Int = 8`)
    Span span;
};

struct StructDecl {
    std::string name;
    std::vector<Param> fields;
    bool isExported = false;
    Span span;
};

struct ConstDecl {
    std::string name;
    std::optional<TypeRef> typeAnnot;
    ExprPtr value;
    Span span;
};

struct EnumDecl {
    std::string name;
    std::vector<std::string> variants;
    std::vector<Span> variantSpans;
    Span span;
};

struct StateDecl {
    std::string name;
    std::optional<TypeRef> typeAnnot;
    ExprPtr init;
    bool isPersistent = false;
    Span span;
};

struct DerivedDecl {
    std::string name;
    std::optional<TypeRef> typeAnnot;
    ExprPtr expr;
    Span span;
};

struct FnDecl {
    std::string name;
    std::vector<Param> params;
    std::optional<TypeRef> returnType;
    std::vector<StmtPtr> body;
    bool isExported = false;
    Span span;
};

struct ComponentDecl {
    std::string name;
    std::vector<Param> params;
    ViewNodePtr view;
    bool isExported = false;
    Span span;
};

struct PluginDecl {
    std::string name;
    std::string version;
    std::string description;
    std::string requiresTransom;
    std::vector<std::pair<std::string, std::string>> requiresPlugins;
    std::vector<std::string> permissions;
    bool hasVersion = false;
    bool hasPermissions = false;

    std::vector<StmtPtr> onLoad;
    std::vector<StmtPtr> onUnload;

    std::vector<StructDecl> structs;
    std::vector<FnDecl> functions;
    std::vector<ComponentDecl> components;
    std::string sourceHash;
    Span span;
};

struct TimerDecl {
    ExprPtr intervalMs;
    std::vector<StmtPtr> body;
    Span span;
};

struct AppDecl {
    std::string title;
    std::vector<PropArg> windowProps;
    std::vector<StateDecl> states;
    std::vector<DerivedDecl> deriveds;
    std::vector<FnDecl> functions;
    std::vector<TimerDecl> timers;
    ViewNodePtr view;
    Span span;
};

struct ImportDecl {
    std::string path;
    Span span;
};

struct UseDecl {
    std::string plugin;
    Span span;
};

struct StyleImportDecl {
    std::string path;
    Span span;
};

struct Program {
    std::vector<ImportDecl> imports;
    std::vector<UseDecl> uses;
    std::vector<StyleImportDecl> styleImports;
    CompiledStyleSheet styleSheet;
    std::vector<PluginDecl> tplugs;
    std::vector<ConstDecl> consts;
    std::vector<StructDecl> structs;
    std::vector<EnumDecl> enums;
    std::vector<FnDecl> functions;
    std::vector<ComponentDecl> components;
    std::optional<AppDecl> app;
};

} // namespace transom
