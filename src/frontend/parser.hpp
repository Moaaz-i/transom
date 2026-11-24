#pragma once

#include "common/source.hpp"
#include "frontend/ast.hpp"
#include "frontend/token.hpp"
#include <vector>

namespace transom {

class Parser {
public:
    Parser(std::vector<Token> tokens, DiagnosticEngine& diag);

    Program parseProgram();

private:
    std::vector<Token> tokens_;
    DiagnosticEngine& diag_;
    size_t pos_ = 0;

    bool isAtEnd() const;
    const Token& peek(size_t offset = 0) const;
    const Token& previous() const;
    const Token& advance();
    bool check(TokenKind kind) const;
    bool match(TokenKind kind);
    void skipSeparators();
    const Token& expect(TokenKind kind, const std::string& msg);
    void synchronize();

    // Top-level
    PluginDecl parsePluginDecl();
    ConstDecl parseConstDecl();
    StructDecl parseStructDecl();
    EnumDecl parseEnumDecl();
    StmtPtr parseMatchStmt();
    StmtPtr parseForStmt();
    AppDecl parseAppDecl(Program& prog);
    ComponentDecl parseComponentDecl();
    FnDecl parseFnDecl();
    StateDecl parseStateDecl();
    DerivedDecl parseDerivedDecl();
    std::vector<Param> parseParamList();
    TypeRef parseTypeRef();

    // Views
    ViewNodePtr parseViewNode();
    ViewNodePtr parseIfView();
    ViewNodePtr parseForView();
    std::vector<ViewNodePtr> parseViewBlock();
    std::vector<PropArg> parsePropArgs();

    // Statements
    std::vector<StmtPtr> parseStmtBlock();
    StmtPtr parseStmt();
    StmtPtr parseLetStmt();
    StmtPtr parseIfStmt();
    StmtPtr parseWhileStmt();
    StmtPtr parseReturnStmt();

    // Expressions
    ExprPtr parseExpr();
    ExprPtr parseOr();
    ExprPtr parseAnd();
    ExprPtr parseEquality();
    ExprPtr parseComparison();
    ExprPtr parseTerm();
    ExprPtr parseFactor();
    ExprPtr parseUnary();
    ExprPtr parsePostfix();
    ExprPtr parsePrimary();
    ExprPtr parsePipe();
    ExprPtr parseCoalesce();
    ExprPtr parseIfExpr();
    ExprPtr parseMatchExpr();
    ExprPtr parseInterpString();
};

} // namespace transom
