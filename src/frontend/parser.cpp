#include "frontend/parser.hpp"

#include <cstdlib>

namespace transom {

namespace {

std::string extractRootVar(const Expr& e) {
    if (auto* id = std::get_if<IdentExpr>(&e.node)) return id->name;
    if (auto* idx = std::get_if<IndexExpr>(&e.node)) return extractRootVar(*idx->object);
    if (auto* mem = std::get_if<MemberExpr>(&e.node)) return extractRootVar(*mem->object);
    return "";
}

} // namespace

Parser::Parser(std::vector<Token> tokens, DiagnosticEngine& diag)
    : tokens_(std::move(tokens)), diag_(diag) {}

bool Parser::isAtEnd() const {
    return pos_ >= tokens_.size() || tokens_[pos_].kind == TokenKind::Eof;
}

const Token& Parser::peek(size_t offset) const {
    size_t idx = pos_ + offset;
    if (idx >= tokens_.size()) return tokens_.back();
    return tokens_[idx];
}

const Token& Parser::previous() const {
    return tokens_[pos_ > 0 ? pos_ - 1 : 0];
}

const Token& Parser::advance() {
    if (!isAtEnd()) pos_++;
    return previous();
}

bool Parser::check(TokenKind kind) const {
    if (isAtEnd()) return kind == TokenKind::Eof;
    return peek().kind == kind;
}

bool Parser::match(TokenKind kind) {
    if (!check(kind)) return false;
    advance();
    return true;
}

void Parser::skipSeparators() {
    while (match(TokenKind::Newline) || match(TokenKind::Semicolon)) {}
}

const Token& Parser::expect(TokenKind kind, const std::string& msg) {
    if (check(kind)) return advance();
    diag_.error(peek().span, msg + " (got '" + peek().lexeme + "')");
    return peek();
}

void Parser::synchronize() {
    if (!isAtEnd() && !check(TokenKind::RBrace)) {
        advance();
    }
    while (!isAtEnd()) {
        if (match(TokenKind::Newline) || match(TokenKind::Semicolon)) return;
        switch (peek().kind) {
            case TokenKind::KwApp:
            case TokenKind::KwComponent:
            case TokenKind::KwConst:
            case TokenKind::KwStruct:
            case TokenKind::KwEnum:
            case TokenKind::KwState:
            case TokenKind::KwDerived:
            case TokenKind::KwView:
            case TokenKind::KwFn:
            case TokenKind::RBrace:
                return;
            default:
                advance();
                break;
        }
    }
}

Program Parser::parseProgram() {
    Program prog;
    skipSeparators();
    while (!isAtEnd()) {
        size_t before = pos_;
        if (check(TokenKind::KwImport)) {
            Span sp = advance().span;
            std::string path = expect(TokenKind::String, "Expected module path string after 'import'").lexeme;
            prog.imports.push_back({std::move(path), sp});
        } else if (check(TokenKind::KwUse)) {
            Span sp = advance().span;
            std::string plugin = expect(TokenKind::String, "Expected plugin name or path string after 'use'").lexeme;
            prog.uses.push_back({std::move(plugin), sp});
        } else if (check(TokenKind::KwStyle)) {
            Span sp = advance().span;
            std::string spath = expect(TokenKind::String, "Expected .tstyle path string after 'style'").lexeme;
            prog.styleImports.push_back({std::move(spath), sp});
        } else if (check(TokenKind::KwPlugin)) {
            prog.tplugs.push_back(parsePluginDecl());
        } else if (check(TokenKind::KwApp)) {
            if (prog.app.has_value()) {
                diag_.error(peek().span, "Only one 'app' declaration is allowed per file");
            }
            prog.app = parseAppDecl(prog);
        } else if (check(TokenKind::KwConst)) {
            prog.consts.push_back(parseConstDecl());
        } else if (check(TokenKind::KwStruct)) {
            prog.structs.push_back(parseStructDecl());
        } else if (check(TokenKind::KwEnum)) {
            prog.enums.push_back(parseEnumDecl());
        } else if (check(TokenKind::KwComponent)) {
            prog.components.push_back(parseComponentDecl());
        } else if (check(TokenKind::KwFn)) {
            prog.functions.push_back(parseFnDecl());
        } else if (check(TokenKind::KwState) || check(TokenKind::KwDerived) || check(TokenKind::KwView)) {
            diag_.error(peek().span,
                        "'" + peek().lexeme + "' must be declared inside an 'app' block, not at top level");
            synchronize();
        } else if (check(TokenKind::KwLet)) {
            diag_.error(peek().span,
                        "'let' is only allowed inside a function ('fn'); use 'const' at top level or 'state' inside 'app'");
            (void)parseLetStmt();
        } else {
            diag_.error(peek().span,
                        "Expected 'app', 'struct', 'enum', 'const', 'component', or 'fn' at top level (got '" +
                            peek().lexeme + "')");
            synchronize();
        }
        if (pos_ == before) advance();
        skipSeparators();
    }
    return prog;
}

PluginDecl Parser::parsePluginDecl() {
    PluginDecl pl;
    pl.span = expect(TokenKind::KwPlugin, "Expected 'plugin'").span;
    pl.name = expect(TokenKind::String, "Expected plugin name string").lexeme;

    skipSeparators();
    expect(TokenKind::LBrace, "Expected '{' to start plugin body");
    skipSeparators();

    while (!isAtEnd() && !check(TokenKind::RBrace)) {
        size_t before = pos_;
        if (check(TokenKind::KwExport)) {
            advance();
            if (check(TokenKind::KwFn)) {
                auto fn = parseFnDecl();
                fn.isExported = true;
                pl.functions.push_back(std::move(fn));
            } else if (check(TokenKind::KwStruct)) {
                auto sd = parseStructDecl();
                sd.isExported = true;
                pl.structs.push_back(std::move(sd));
            } else if (check(TokenKind::KwComponent)) {
                auto cd = parseComponentDecl();
                cd.isExported = true;
                pl.components.push_back(std::move(cd));
            } else {
                diag_.error(peek().span, "Expected 'fn', 'struct', or 'component' after 'export'");
                synchronize();
            }
        } else if (check(TokenKind::KwFn)) {
            pl.functions.push_back(parseFnDecl());
        } else if (check(TokenKind::KwStruct)) {
            pl.structs.push_back(parseStructDecl());
        } else if (check(TokenKind::KwComponent)) {
            pl.components.push_back(parseComponentDecl());
        } else if (check(TokenKind::Ident)) {
            std::string key = advance().lexeme;
            if (key == "onLoad") {
                if (match(TokenKind::LParen)) expect(TokenKind::RParen, "Expected ')'");
                pl.onLoad = parseStmtBlock();
            } else if (key == "onUnload") {
                if (match(TokenKind::LParen)) expect(TokenKind::RParen, "Expected ')'");
                pl.onUnload = parseStmtBlock();
            } else {
                expect(TokenKind::Colon, "Expected ':' after plugin metadata field '" + key + "'");
                if (key == "version") {
                    pl.version = expect(TokenKind::String, "Expected version string").lexeme;
                    pl.hasVersion = true;
                } else if (key == "description") {
                    pl.description = expect(TokenKind::String, "Expected description string").lexeme;
                } else if (key == "permissions") {
                    pl.hasPermissions = true;
                    expect(TokenKind::LBracket, "Expected '[' for permissions list");
                    if (!check(TokenKind::RBracket)) {
                        do {
                            pl.permissions.push_back(
                                expect(TokenKind::String, "Expected permission string").lexeme);
                        } while (match(TokenKind::Comma));
                    }
                    expect(TokenKind::RBracket, "Expected ']' after permissions");
                } else if (key == "requires") {
                    skipSeparators();
                    expect(TokenKind::LBrace, "Expected '{' for requires block");
                    skipSeparators();
                    while (!isAtEnd() && !check(TokenKind::RBrace)) {
                        std::string rkey = expect(TokenKind::Ident, "Expected 'transom' or 'plugins'").lexeme;
                        expect(TokenKind::Colon, "Expected ':'");
                        if (rkey == "transom") {
                            pl.requiresTransom = expect(TokenKind::String, "Expected semver string").lexeme;
                        } else if (rkey == "plugins") {
                            skipSeparators();
                            expect(TokenKind::LBrace, "Expected '{' for plugins dependencies");
                            skipSeparators();
                            while (!isAtEnd() && !check(TokenKind::RBrace)) {
                                std::string depName = expect(TokenKind::String, "Expected plugin name string").lexeme;
                                expect(TokenKind::Colon, "Expected ':'");
                                std::string depVer = expect(TokenKind::String, "Expected semver constraint").lexeme;
                                pl.requiresPlugins.emplace_back(std::move(depName), std::move(depVer));
                                match(TokenKind::Comma);
                                skipSeparators();
                            }
                            expect(TokenKind::RBrace, "Expected '}' after plugins dependencies");
                        }
                        match(TokenKind::Comma);
                        skipSeparators();
                    }
                    expect(TokenKind::RBrace, "Expected '}' after requires block");
                } else {
                    diag_.error(previous().span, "Unknown plugin metadata field '" + key + "'");
                    synchronize();
                }
            }
        } else {
            diag_.error(peek().span, "Unexpected token inside plugin block");
            synchronize();
        }
        if (pos_ == before) advance();
        skipSeparators();
    }

    expect(TokenKind::RBrace, "Expected '}' to close plugin body");
    return pl;
}

ConstDecl Parser::parseConstDecl() {
    ConstDecl cd;
    cd.span = expect(TokenKind::KwConst, "Expected 'const'").span;
    cd.name = expect(TokenKind::Ident, "Expected constant name").lexeme;
    if (match(TokenKind::Colon)) {
        cd.typeAnnot = parseTypeRef();
    }
    expect(TokenKind::Equal, "Expected '=' in const declaration");
    cd.value = parseExpr();
    return cd;
}

StructDecl Parser::parseStructDecl() {
    StructDecl sd;
    sd.span = expect(TokenKind::KwStruct, "Expected 'struct'").span;
    sd.name = expect(TokenKind::Ident, "Expected struct name").lexeme;
    skipSeparators();
    expect(TokenKind::LBrace, "Expected '{' to start struct fields");
    skipSeparators();
    while (!isAtEnd() && !check(TokenKind::RBrace)) {
        size_t before = pos_;
        Param f;
        const Token& nameTok = expect(TokenKind::Ident, "Expected field name");
        f.name = nameTok.lexeme;
        f.span = nameTok.span;
        expect(TokenKind::Colon, "Expected ':' after field name");
        f.type = parseTypeRef();
        sd.fields.push_back(std::move(f));
        match(TokenKind::Comma);
        if (pos_ == before) advance();
        skipSeparators();
    }
    expect(TokenKind::RBrace, "Expected '}' to close struct");
    return sd;
}

EnumDecl Parser::parseEnumDecl() {
    EnumDecl ed;
    ed.span = expect(TokenKind::KwEnum, "Expected 'enum'").span;
    ed.name = expect(TokenKind::Ident, "Expected enum name").lexeme;
    skipSeparators();
    expect(TokenKind::LBrace, "Expected '{' to start enum variants");
    skipSeparators();
    while (!isAtEnd() && !check(TokenKind::RBrace)) {
        size_t before = pos_;
        const Token& t = expect(TokenKind::Ident, "Expected enum variant name");
        ed.variants.push_back(t.lexeme);
        ed.variantSpans.push_back(t.span);
        match(TokenKind::Comma);
        if (pos_ == before) advance();
        skipSeparators();
    }
    expect(TokenKind::RBrace, "Expected '}' to close enum");
    return ed;
}

StmtPtr Parser::parseMatchStmt() {
    Span sp = expect(TokenKind::KwMatch, "Expected 'match'").span;
    MatchStmt m;
    m.scrutinee = parseExpr();
    skipSeparators();
    expect(TokenKind::LBrace, "Expected '{' to start match arms");
    skipSeparators();
    while (!isAtEnd() && !check(TokenKind::RBrace)) {
        size_t before = pos_;
        if (match(TokenKind::KwElse)) {
            if (m.hasElse) diag_.error(previous().span, "Duplicate 'else' arm in match");
            m.hasElse = true;
            if (match(TokenKind::FatArrow)) {
                m.elseBody.push_back(parseStmt());
            } else {
                m.elseBody = parseStmtBlock();
            }
        } else {
            MatchArm arm;
            arm.span = peek().span;
            arm.pattern = parseExpr();
            if (match(TokenKind::FatArrow)) {
                arm.body.push_back(parseStmt());
            } else {
                arm.body = parseStmtBlock();
            }
            m.arms.push_back(std::move(arm));
        }
        match(TokenKind::Comma);
        if (pos_ == before) advance();
        skipSeparators();
    }
    expect(TokenKind::RBrace, "Expected '}' to close match");
    auto stmt = std::make_unique<Stmt>();
    stmt->span = sp;
    stmt->node = std::move(m);
    return stmt;
}

StmtPtr Parser::parseForStmt() {
    Span sp = expect(TokenKind::KwFor, "Expected 'for'").span;
    ForStmt fs;
    if (match(TokenKind::LParen)) {
        fs.indexVar = expect(TokenKind::Ident, "Expected loop index variable").lexeme;
        expect(TokenKind::Comma, "Expected ',' between index and item variables");
        fs.itemVar = expect(TokenKind::Ident, "Expected loop item variable").lexeme;
        expect(TokenKind::RParen, "Expected ')' after loop variables");
    } else {
        fs.itemVar = expect(TokenKind::Ident, "Expected loop item variable").lexeme;
    }
    expect(TokenKind::KwIn, "Expected 'in' in for statement");
    fs.iterable = parseExpr();
    fs.body = parseStmtBlock();
    auto stmt = std::make_unique<Stmt>();
    stmt->span = sp;
    stmt->node = std::move(fs);
    return stmt;
}

AppDecl Parser::parseAppDecl(Program& prog) {
    AppDecl app;
    app.span = expect(TokenKind::KwApp, "Expected 'app'").span;

    const Token& titleTok = expect(TokenKind::String, "Expected app title string");
    app.title = titleTok.lexeme;

    if (check(TokenKind::LParen)) {
        app.windowProps = parsePropArgs();
    }

    skipSeparators();
    expect(TokenKind::LBrace, "Expected '{' to start app body");
    skipSeparators();

    while (!isAtEnd() && !check(TokenKind::RBrace)) {
        size_t before = pos_;
        if (check(TokenKind::KwPersist)) {
            advance();
            StateDecl st = parseStateDecl();
            st.isPersistent = true;
            app.states.push_back(std::move(st));
        } else if (check(TokenKind::KwState)) {
            app.states.push_back(parseStateDecl());
        } else if (check(TokenKind::KwDerived)) {
            app.deriveds.push_back(parseDerivedDecl());
        } else if (check(TokenKind::KwFn)) {
            app.functions.push_back(parseFnDecl());
        } else if (check(TokenKind::KwEvery)) {
            TimerDecl td;
            td.span = advance().span;
            expect(TokenKind::LParen, "Expected '(' after 'every'");
            td.intervalMs = parseExpr();
            expect(TokenKind::RParen, "Expected ')' after timer interval");
            td.body = parseStmtBlock();
            app.timers.push_back(std::move(td));
        } else if (check(TokenKind::KwView)) {
            advance();
            if (app.view) {
                diag_.error(previous().span, "App already has a 'view' declaration");
            }
            app.view = parseViewNode();
        } else if (check(TokenKind::KwComponent)) {
            diag_.error(peek().span,
                        "'component' must be declared at top level (outside 'app')");
            prog.components.push_back(parseComponentDecl());
        } else if (check(TokenKind::KwStruct)) {
            diag_.error(peek().span,
                        "'struct' must be declared at top level (outside 'app')");
            prog.structs.push_back(parseStructDecl());
        } else if (check(TokenKind::KwEnum)) {
            diag_.error(peek().span,
                        "'enum' must be declared at top level (outside 'app')");
            prog.enums.push_back(parseEnumDecl());
        } else if (check(TokenKind::KwConst)) {
            diag_.error(peek().span,
                        "'const' must be declared at top level (outside 'app')");
            prog.consts.push_back(parseConstDecl());
        } else if (check(TokenKind::KwLet)) {
            diag_.error(peek().span,
                        "Use 'state' or 'derived' instead of 'let' inside 'app' ('let' is only for local variables inside 'fn')");
            (void)parseLetStmt();
        } else {
            diag_.error(peek().span,
                        "Expected 'state', 'derived', 'fn', 'every', or 'view' inside app (got '" +
                            peek().lexeme + "')");
            synchronize();
        }
        if (pos_ == before) advance();
        skipSeparators();
    }

    expect(TokenKind::RBrace, "Expected '}' to close app body");
    return app;
}

ComponentDecl Parser::parseComponentDecl() {
    ComponentDecl comp;
    comp.span = expect(TokenKind::KwComponent, "Expected 'component'").span;
    comp.name = expect(TokenKind::Ident, "Expected component name").lexeme;
    if (check(TokenKind::LParen)) {
        comp.params = parseParamList();
    }
    skipSeparators();
    expect(TokenKind::LBrace, "Expected '{' to start component body");
    skipSeparators();
    if (match(TokenKind::KwView)) {
        comp.view = parseViewNode();
    } else if (check(TokenKind::Ident) || check(TokenKind::KwIf) || check(TokenKind::KwFor) ||
               check(TokenKind::KwSlot)) {
        comp.view = parseViewNode();
    } else if (check(TokenKind::KwReturn)) {
        diag_.error(peek().span,
                    "Component '" + comp.name +
                        "' must contain a UI widget tree (not 'return'); use 'fn' for value-returning functions");
        advance();
        if (!check(TokenKind::RBrace) && !check(TokenKind::Newline) && !check(TokenKind::Semicolon)) {
            (void)parseExpr();
        }
    } else if (!check(TokenKind::RBrace)) {
        diag_.error(peek().span, "Expected UI widget or 'view' inside component '" + comp.name + "'");
    }
    skipSeparators();
    int braceDepth = 0;
    while (!isAtEnd() && (braceDepth > 0 || !check(TokenKind::RBrace))) {
        if (check(TokenKind::LBrace)) braceDepth++;
        else if (check(TokenKind::RBrace)) braceDepth--;
        advance();
    }
    expect(TokenKind::RBrace, "Expected '}' to close component body");
    return comp;
}

std::vector<Param> Parser::parseParamList() {
    std::vector<Param> params;
    expect(TokenKind::LParen, "Expected '('");
    if (!check(TokenKind::RParen)) {
        do {
            Param p;
            const Token& nameTok = expect(TokenKind::Ident, "Expected parameter name");
            p.name = nameTok.lexeme;
            p.span = nameTok.span;
            expect(TokenKind::Colon, "Expected ':' after parameter name");
            p.type = parseTypeRef();
            if (match(TokenKind::Equal)) {
                p.defaultValue = parseExpr();
            }
            params.push_back(std::move(p));
        } while (match(TokenKind::Comma));
    }
    expect(TokenKind::RParen, "Expected ')'");
    return params;
}

TypeRef Parser::parseTypeRef() {
    const Token& tok = expect(TokenKind::Ident, "Expected type name");
    TypeRef ref;
    ref.name = tok.lexeme;
    ref.span = tok.span;
    if (match(TokenKind::Less)) {
        do {
            ref.typeArgs.push_back(parseTypeRef());
        } while (match(TokenKind::Comma));
        expect(TokenKind::Greater, "Expected '>' after type arguments");
    }
    return ref;
}

StateDecl Parser::parseStateDecl() {
    StateDecl decl;
    decl.span = expect(TokenKind::KwState, "Expected 'state'").span;
    decl.name = expect(TokenKind::Ident, "Expected state variable name").lexeme;
    if (match(TokenKind::Colon)) {
        decl.typeAnnot = parseTypeRef();
    }
    expect(TokenKind::Equal, "Expected '=' in state declaration");
    decl.init = parseExpr();
    return decl;
}

DerivedDecl Parser::parseDerivedDecl() {
    DerivedDecl decl;
    decl.span = expect(TokenKind::KwDerived, "Expected 'derived'").span;
    decl.name = expect(TokenKind::Ident, "Expected derived variable name").lexeme;
    if (match(TokenKind::Colon)) {
        decl.typeAnnot = parseTypeRef();
    }
    expect(TokenKind::Equal, "Expected '=' in derived declaration");
    decl.expr = parseExpr();
    return decl;
}

FnDecl Parser::parseFnDecl() {
    FnDecl fn;
    fn.span = expect(TokenKind::KwFn, "Expected 'fn'").span;
    fn.name = expect(TokenKind::Ident, "Expected function name").lexeme;
    fn.params = parseParamList();
    if (match(TokenKind::Arrow)) {
        fn.returnType = parseTypeRef();
    }
    fn.body = parseStmtBlock();
    return fn;
}

std::vector<PropArg> Parser::parsePropArgs() {
    std::vector<PropArg> props;
    expect(TokenKind::LParen, "Expected '('");
    if (!check(TokenKind::RParen)) {
        do {
            PropArg arg;
            arg.span = peek().span;
            if (check(TokenKind::Ident) && peek(1).kind == TokenKind::Colon) {
                arg.name = advance().lexeme;
                advance(); // ':'
            } else if (check(TokenKind::KwStyle) && peek(1).kind == TokenKind::Colon) {
                arg.name = advance().lexeme;
                advance(); // ':'
            }
            arg.value = parseExpr();
            props.push_back(std::move(arg));
        } while (match(TokenKind::Comma));
    }
    expect(TokenKind::RParen, "Expected ')'");
    return props;
}

std::vector<ViewNodePtr> Parser::parseViewBlock() {
    std::vector<ViewNodePtr> children;
    skipSeparators();
    expect(TokenKind::LBrace, "Expected '{'");
    skipSeparators();
    while (!isAtEnd() && !check(TokenKind::RBrace)) {
        size_t before = pos_;
        children.push_back(parseViewNode());
        if (pos_ == before) advance();
        skipSeparators();
    }
    expect(TokenKind::RBrace, "Expected '}'");
    return children;
}

ViewNodePtr Parser::parseIfView() {
    auto node = std::make_unique<ViewNode>();
    node->kind = ViewNodeKind::If;
    node->span = expect(TokenKind::KwIf, "Expected 'if'").span;
    node->condition = parseOr();
    node->children = parseViewBlock();
    if (match(TokenKind::KwElse)) {
        if (check(TokenKind::KwIf)) {
            node->elseChildren.push_back(parseIfView());
        } else {
            node->elseChildren = parseViewBlock();
        }
    }
    return node;
}

ViewNodePtr Parser::parseForView() {
    auto node = std::make_unique<ViewNode>();
    node->kind = ViewNodeKind::For;
    node->span = expect(TokenKind::KwFor, "Expected 'for'").span;
    if (match(TokenKind::LParen)) {
        node->indexVar = expect(TokenKind::Ident, "Expected loop index variable").lexeme;
        expect(TokenKind::Comma, "Expected ',' between index and item variables");
        node->itemVar = expect(TokenKind::Ident, "Expected loop item variable").lexeme;
        expect(TokenKind::RParen, "Expected ')' after loop variables");
    } else {
        node->itemVar = expect(TokenKind::Ident, "Expected loop item variable").lexeme;
    }
    expect(TokenKind::KwIn, "Expected 'in' in for view");
    node->iterable = parseOr();
    node->children = parseViewBlock();
    return node;
}

ViewNodePtr Parser::parseViewNode() {
    if (check(TokenKind::KwIf)) return parseIfView();
    if (check(TokenKind::KwFor)) return parseForView();
    if (match(TokenKind::KwSlot)) {
        auto node = std::make_unique<ViewNode>();
        node->kind = ViewNodeKind::Slot;
        node->span = previous().span;
        return node;
    }

    auto node = std::make_unique<ViewNode>();
    node->kind = ViewNodeKind::Widget;
    const Token& nameTok = expect(TokenKind::Ident, "Expected widget name");
    node->widget = nameTok.lexeme;
    node->span = nameTok.span;
    if (match(TokenKind::Dot)) {
        node->widget += "." + expect(TokenKind::Ident, "Expected component name after '.'").lexeme;
    }

    if (check(TokenKind::LParen)) {
        node->props = parsePropArgs();
    }

    if (check(TokenKind::LBrace)) {
        // Determine whether '{ ... }' is an action statement block (Button or native event widget)
        // vs a child view block (VStack, HStack, Card, Grid, ZStack, Modal, ScrollView, custom component).
        bool isContainerWidget =
            (node->widget == "VStack" || node->widget == "HStack" || node->widget == "Card" ||
             node->widget == "ScrollView" || node->widget == "Grid" || node->widget == "ZStack" ||
             node->widget == "Modal");
        if (node->widget == "Button") {
            node->action = parseStmtBlock();
        } else if (!isContainerWidget) {
            // Peek first non-newline token inside '{ ... }'
            size_t look = pos_ + 1;
            while (look < tokens_.size() &&
                   (tokens_[look].kind == TokenKind::Newline || tokens_[look].kind == TokenKind::Semicolon)) {
                look++;
            }
            bool looksLikeAction = false;
            if (look < tokens_.size()) {
                TokenKind tk = tokens_[look].kind;
                if (tk == TokenKind::KwLet || tk == TokenKind::KwWhile || tk == TokenKind::KwFor ||
                    tk == TokenKind::KwMatch || tk == TokenKind::KwReturn) {
                    looksLikeAction = true;
                } else if (tk == TokenKind::Ident && look + 1 < tokens_.size()) {
                    TokenKind nk = tokens_[look + 1].kind;
                    if (nk == TokenKind::Equal || nk == TokenKind::PlusEqual ||
                        nk == TokenKind::MinusEqual || nk == TokenKind::StarEqual ||
                        nk == TokenKind::SlashEqual) {
                        looksLikeAction = true;
                    }
                }
            }
            if (looksLikeAction) {
                node->action = parseStmtBlock();
            } else {
                node->children = parseViewBlock();
            }
        } else {
            node->children = parseViewBlock();
        }
    }
    return node;
}

std::vector<StmtPtr> Parser::parseStmtBlock() {
    std::vector<StmtPtr> stmts;
    skipSeparators();
    expect(TokenKind::LBrace, "Expected '{'");
    skipSeparators();
    while (!isAtEnd() && !check(TokenKind::RBrace)) {
        size_t before = pos_;
        stmts.push_back(parseStmt());
        if (pos_ == before) advance();
        skipSeparators();
    }
    expect(TokenKind::RBrace, "Expected '}'");
    return stmts;
}

StmtPtr Parser::parseStmt() {
    if (check(TokenKind::KwLet)) return parseLetStmt();
    if (check(TokenKind::KwIf)) return parseIfStmt();
    if (check(TokenKind::KwWhile)) return parseWhileStmt();
    if (check(TokenKind::KwFor)) return parseForStmt();
    if (check(TokenKind::KwMatch)) return parseMatchStmt();
    if (match(TokenKind::KwBreak)) {
        auto stmt = std::make_unique<Stmt>();
        stmt->span = previous().span;
        stmt->node = BreakStmt{};
        return stmt;
    }
    if (match(TokenKind::KwContinue)) {
        auto stmt = std::make_unique<Stmt>();
        stmt->span = previous().span;
        stmt->node = ContinueStmt{};
        return stmt;
    }
    if (check(TokenKind::KwReturn)) return parseReturnStmt();

    Span sp = peek().span;
    auto lhs = parseExpr();

    // Check for assignment (=, +=, -=, *=, /=)
    if (match(TokenKind::Equal) || match(TokenKind::PlusEqual) ||
        match(TokenKind::MinusEqual) || match(TokenKind::StarEqual) ||
        match(TokenKind::SlashEqual)) {
        std::string op = previous().lexeme;
        auto rhs = parseExpr();

        if (auto* id = std::get_if<IdentExpr>(&lhs->node)) {
            auto stmt = std::make_unique<Stmt>();
            stmt->span = sp;
            stmt->node = AssignStmt{id->name, std::move(op), std::move(rhs)};
            return stmt;
        }
        if (auto* idx = std::get_if<IndexExpr>(&lhs->node)) {
            if (op != "=") {
                diag_.error(sp, "Compound assignment on list index is not supported yet; use '='");
            }
            std::string root = extractRootVar(*idx->object);
            auto stmt = std::make_unique<Stmt>();
            stmt->span = sp;
            stmt->node = IndexAssignStmt{std::move(idx->object), std::move(root),
                                         std::move(idx->index), std::move(rhs)};
            return stmt;
        }
        if (auto* mem = std::get_if<MemberExpr>(&lhs->node)) {
            if (op != "=") {
                diag_.error(sp, "Compound assignment on struct field is not supported yet; use '='");
            }
            std::string root = extractRootVar(*mem->object);
            auto stmt = std::make_unique<Stmt>();
            stmt->span = sp;
            stmt->node = FieldAssignStmt{std::move(mem->object), std::move(root),
                                         std::move(mem->field), std::move(rhs)};
            return stmt;
        }
        diag_.error(sp, "Invalid assignment target");
    }

    auto stmt = std::make_unique<Stmt>();
    stmt->span = sp;
    stmt->node = ExprStmt{std::move(lhs)};
    return stmt;
}

StmtPtr Parser::parseLetStmt() {
    Span sp = expect(TokenKind::KwLet, "Expected 'let'").span;
    std::string name = expect(TokenKind::Ident, "Expected variable name").lexeme;
    std::optional<TypeRef> annot;
    if (match(TokenKind::Colon)) {
        annot = parseTypeRef();
    }
    expect(TokenKind::Equal, "Expected '=' in let statement");
    auto init = parseExpr();
    auto stmt = std::make_unique<Stmt>();
    stmt->span = sp;
    stmt->node = LetStmt{std::move(name), std::move(annot), std::move(init)};
    return stmt;
}

StmtPtr Parser::parseIfStmt() {
    Span sp = expect(TokenKind::KwIf, "Expected 'if'").span;
    auto cond = parseExpr();
    auto thenBody = parseStmtBlock();
    std::vector<StmtPtr> elseBody;
    if (match(TokenKind::KwElse)) {
        if (check(TokenKind::KwIf)) {
            elseBody.push_back(parseIfStmt());
        } else {
            elseBody = parseStmtBlock();
        }
    }
    auto stmt = std::make_unique<Stmt>();
    stmt->span = sp;
    stmt->node = IfStmt{std::move(cond), std::move(thenBody), std::move(elseBody)};
    return stmt;
}

StmtPtr Parser::parseWhileStmt() {
    Span sp = expect(TokenKind::KwWhile, "Expected 'while'").span;
    auto cond = parseExpr();
    auto body = parseStmtBlock();
    auto stmt = std::make_unique<Stmt>();
    stmt->span = sp;
    stmt->node = WhileStmt{std::move(cond), std::move(body)};
    return stmt;
}

StmtPtr Parser::parseReturnStmt() {
    Span sp = expect(TokenKind::KwReturn, "Expected 'return'").span;
    ExprPtr val;
    if (!check(TokenKind::Newline) && !check(TokenKind::Semicolon) &&
        !check(TokenKind::RBrace) && !isAtEnd()) {
        val = parseExpr();
    }
    auto stmt = std::make_unique<Stmt>();
    stmt->span = sp;
    stmt->node = ReturnStmt{std::move(val)};
    return stmt;
}

ExprPtr Parser::parseExpr() {
    if (check(TokenKind::KwIf)) return parseIfExpr();
    if (check(TokenKind::KwMatch)) return parseMatchExpr();
    return parsePipe();
}

ExprPtr Parser::parseMatchExpr() {
    Span sp = expect(TokenKind::KwMatch, "Expected 'match'").span;
    MatchExpr m;
    m.scrutinee = parsePipe();
    skipSeparators();
    expect(TokenKind::LBrace, "Expected '{' in match expression");
    skipSeparators();
    while (!isAtEnd() && !check(TokenKind::RBrace)) {
        size_t before = pos_;
        if (match(TokenKind::KwElse)) {
            match(TokenKind::FatArrow);
            if (match(TokenKind::LBrace)) {
                skipSeparators();
                m.elseExpr = parseExpr();
                skipSeparators();
                expect(TokenKind::RBrace, "Expected '}' after else expression");
            } else {
                m.elseExpr = parseExpr();
            }
        } else {
            MatchExprArm arm;
            arm.span = peek().span;
            arm.pattern = parseCoalesce();
            expect(TokenKind::FatArrow, "Expected '=>' in match expression arm");
            if (match(TokenKind::LBrace)) {
                skipSeparators();
                arm.value = parseExpr();
                skipSeparators();
                expect(TokenKind::RBrace, "Expected '}' after match arm expression");
            } else {
                arm.value = parseExpr();
            }
            m.arms.push_back(std::move(arm));
        }
        match(TokenKind::Comma);
        if (pos_ == before) advance();
        skipSeparators();
    }
    expect(TokenKind::RBrace, "Expected '}' to close match expression");
    if (!m.elseExpr) {
        auto nullE = std::make_unique<Expr>();
        nullE->span = sp;
        nullE->node = NullLitExpr{};
        m.elseExpr = std::move(nullE);
    }
    auto e = std::make_unique<Expr>();
    e->span = sp;
    e->node = std::move(m);
    return e;
}

ExprPtr Parser::parsePipe() {
    auto left = parseCoalesce();
    while (match(TokenKind::PipeGreater)) {
        Span sp = previous().span;
        auto rhs = parseCoalesce();
        // Desugar `x |> fn(a, b)` -> `fn(x, a, b)` and `x |> fn` -> `fn(x)`
        if (auto* call = std::get_if<CallExpr>(&rhs->node)) {
            call->args.insert(call->args.begin(), std::move(left));
            left = std::move(rhs);
        } else if (auto* mcall = std::get_if<MethodCallExpr>(&rhs->node)) {
            mcall->args.insert(mcall->args.begin(), std::move(left));
            left = std::move(rhs);
        } else if (auto* id = std::get_if<IdentExpr>(&rhs->node)) {
            std::string fnName = id->name;
            std::vector<ExprPtr> args;
            args.push_back(std::move(left));
            auto e = std::make_unique<Expr>();
            e->span = sp;
            e->node = CallExpr{std::move(fnName), std::move(args)};
            left = std::move(e);
        } else {
            diag_.error(sp, "Right side of '|>' must be a function or call expression");
        }
    }
    return left;
}

ExprPtr Parser::parseCoalesce() {
    auto left = parseOr();
    while (match(TokenKind::QuestionQuestion)) {
        Span sp = previous().span;
        auto right = parseOr();
        auto e = std::make_unique<Expr>();
        e->span = sp;
        e->node = BinaryExpr{"??", std::move(left), std::move(right)};
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::parseIfExpr() {
    Span sp = expect(TokenKind::KwIf, "Expected 'if'").span;
    auto cond = parseOr();
    expect(TokenKind::LBrace, "Expected '{' in if expression");
    skipSeparators();
    auto thenExpr = parseExpr();
    skipSeparators();
    expect(TokenKind::RBrace, "Expected '}' in if expression");
    expect(TokenKind::KwElse, "Expected 'else' in if expression");
    ExprPtr elseExpr;
    if (check(TokenKind::KwIf)) {
        elseExpr = parseIfExpr();
    } else {
        expect(TokenKind::LBrace, "Expected '{' after 'else'");
        skipSeparators();
        elseExpr = parseExpr();
        skipSeparators();
        expect(TokenKind::RBrace, "Expected '}' after else expression");
    }
    auto e = std::make_unique<Expr>();
    e->span = sp;
    e->node = IfExpr{std::move(cond), std::move(thenExpr), std::move(elseExpr)};
    return e;
}

ExprPtr Parser::parseOr() {
    auto left = parseAnd();
    while (match(TokenKind::PipePipe) || match(TokenKind::KwOr)) {
        Span sp = previous().span;
        auto right = parseAnd();
        auto e = std::make_unique<Expr>();
        e->span = sp;
        e->node = BinaryExpr{"||", std::move(left), std::move(right)};
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::parseAnd() {
    auto left = parseEquality();
    while (match(TokenKind::AmpAmp) || match(TokenKind::KwAnd)) {
        Span sp = previous().span;
        auto right = parseEquality();
        auto e = std::make_unique<Expr>();
        e->span = sp;
        e->node = BinaryExpr{"&&", std::move(left), std::move(right)};
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::parseEquality() {
    auto left = parseComparison();
    while (match(TokenKind::EqualEqual) || match(TokenKind::BangEqual)) {
        const Token& op = previous();
        auto right = parseComparison();
        auto e = std::make_unique<Expr>();
        e->span = op.span;
        e->node = BinaryExpr{op.lexeme, std::move(left), std::move(right)};
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::parseComparison() {
    auto left = parseTerm();
    while (match(TokenKind::Less) || match(TokenKind::LessEqual) ||
           match(TokenKind::Greater) || match(TokenKind::GreaterEqual)) {
        const Token& op = previous();
        auto right = parseTerm();
        auto e = std::make_unique<Expr>();
        e->span = op.span;
        e->node = BinaryExpr{op.lexeme, std::move(left), std::move(right)};
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::parseTerm() {
    auto left = parseFactor();
    while (match(TokenKind::Plus) || match(TokenKind::Minus)) {
        const Token& op = previous();
        auto right = parseFactor();
        auto e = std::make_unique<Expr>();
        e->span = op.span;
        e->node = BinaryExpr{op.lexeme, std::move(left), std::move(right)};
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::parseFactor() {
    auto left = parseUnary();
    while (match(TokenKind::Star) || match(TokenKind::Slash) || match(TokenKind::Percent)) {
        const Token& op = previous();
        auto right = parseUnary();
        auto e = std::make_unique<Expr>();
        e->span = op.span;
        e->node = BinaryExpr{op.lexeme, std::move(left), std::move(right)};
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::parseUnary() {
    if (match(TokenKind::Minus) || match(TokenKind::Bang) || match(TokenKind::KwNot)) {
        const Token& op = previous();
        std::string opStr = (op.kind == TokenKind::KwNot) ? "!" : op.lexeme;
        auto operand = parseUnary();
        auto e = std::make_unique<Expr>();
        e->span = op.span;
        e->node = UnaryExpr{std::move(opStr), std::move(operand)};
        return e;
    }
    return parsePostfix();
}

ExprPtr Parser::parsePostfix() {
    auto expr = parsePrimary();
    while (true) {
        if (match(TokenKind::LBracket)) {
            Span sp = previous().span;
            auto idx = parseExpr();
            expect(TokenKind::RBracket, "Expected ']' after index");
            auto e = std::make_unique<Expr>();
            e->span = sp;
            e->node = IndexExpr{std::move(expr), std::move(idx)};
            expr = std::move(e);
        } else if (match(TokenKind::Dot)) {
            Span sp = previous().span;
            std::string field = expect(TokenKind::Ident, "Expected field or method name after '.'").lexeme;
            if (match(TokenKind::LParen)) {
                std::vector<ExprPtr> args;
                if (!check(TokenKind::RParen)) {
                    do {
                        args.push_back(parseExpr());
                    } while (match(TokenKind::Comma));
                }
                expect(TokenKind::RParen, "Expected ')' after method arguments");
                std::string objName;
                if (auto* id = std::get_if<IdentExpr>(&expr->node)) {
                    objName = id->name;
                }
                auto e = std::make_unique<Expr>();
                e->span = sp;
                e->node = MethodCallExpr{std::move(objName), std::move(field), std::move(args)};
                expr = std::move(e);
            } else {
                auto e = std::make_unique<Expr>();
                e->span = sp;
                e->node = MemberExpr{std::move(expr), std::move(field)};
                expr = std::move(e);
            }
        } else {
            break;
        }
    }
    return expr;
}

ExprPtr Parser::parseInterpString() {
    Span sp = peek().span;
    InterpExpr interp;
    while (!isAtEnd()) {
        if (match(TokenKind::StringPart)) {
            if (!previous().lexeme.empty()) {
                auto lit = std::make_unique<Expr>();
                lit->span = previous().span;
                lit->node = StringLitExpr{previous().lexeme};
                interp.parts.push_back(std::move(lit));
            }
            if (match(TokenKind::InterpStart)) {
                interp.parts.push_back(parseExpr());
                expect(TokenKind::InterpEnd, "Expected '}' to close string interpolation");
            }
        } else if (match(TokenKind::StringEnd)) {
            if (!previous().lexeme.empty()) {
                auto lit = std::make_unique<Expr>();
                lit->span = previous().span;
                lit->node = StringLitExpr{previous().lexeme};
                interp.parts.push_back(std::move(lit));
            }
            break;
        } else {
            diag_.error(peek().span, "Expected end of interpolated string");
            break;
        }
    }
    auto e = std::make_unique<Expr>();
    e->span = sp;
    e->node = std::move(interp);
    return e;
}

ExprPtr Parser::parsePrimary() {
    if (match(TokenKind::Int)) {
        auto e = std::make_unique<Expr>();
        e->span = previous().span;
        e->node = IntLitExpr{std::stoll(previous().lexeme)};
        return e;
    }
    if (match(TokenKind::Float)) {
        auto e = std::make_unique<Expr>();
        e->span = previous().span;
        e->node = FloatLitExpr{std::stod(previous().lexeme)};
        return e;
    }
    if (match(TokenKind::KwTrue) || match(TokenKind::KwFalse)) {
        auto e = std::make_unique<Expr>();
        e->span = previous().span;
        e->node = BoolLitExpr{previous().kind == TokenKind::KwTrue};
        return e;
    }
    if (match(TokenKind::KwNull)) {
        auto e = std::make_unique<Expr>();
        e->span = previous().span;
        e->node = NullLitExpr{};
        return e;
    }
    if (match(TokenKind::String)) {
        auto e = std::make_unique<Expr>();
        e->span = previous().span;
        e->node = StringLitExpr{previous().lexeme};
        return e;
    }
    if (check(TokenKind::StringPart)) {
        return parseInterpString();
    }
    if (match(TokenKind::LBracket)) {
        Span sp = previous().span;
        std::vector<ExprPtr> elems;
        if (!check(TokenKind::RBracket)) {
            do {
                elems.push_back(parseExpr());
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RBracket, "Expected ']' after list literal");
        auto e = std::make_unique<Expr>();
        e->span = sp;
        e->node = ListLitExpr{std::move(elems)};
        return e;
    }
    if (match(TokenKind::Ident)) {
        const Token& id = previous();
        if (match(TokenKind::LParen)) {
            // Struct initializer if `TypeName(field: expr, ...)`
            if (check(TokenKind::Ident) && peek(1).kind == TokenKind::Colon) {
                std::vector<std::pair<std::string, ExprPtr>> fields;
                do {
                    std::string fname = expect(TokenKind::Ident, "Expected field name").lexeme;
                    expect(TokenKind::Colon, "Expected ':' after field name");
                    fields.emplace_back(std::move(fname), parseExpr());
                } while (match(TokenKind::Comma));
                expect(TokenKind::RParen, "Expected ')' after struct fields");
                auto e = std::make_unique<Expr>();
                e->span = id.span;
                e->node = StructInitExpr{id.lexeme, std::move(fields)};
                return e;
            }
            std::vector<ExprPtr> args;
            if (!check(TokenKind::RParen)) {
                do {
                    args.push_back(parseExpr());
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RParen, "Expected ')' after arguments");
            auto e = std::make_unique<Expr>();
            e->span = id.span;
            e->node = CallExpr{id.lexeme, std::move(args)};
            return e;
        }
        auto e = std::make_unique<Expr>();
        e->span = id.span;
        e->node = IdentExpr{id.lexeme};
        return e;
    }
    if (match(TokenKind::LParen)) {
        auto inner = parseExpr();
        expect(TokenKind::RParen, "Expected ')'");
        return inner;
    }

    diag_.error(peek().span, "Expected expression (got '" + peek().lexeme + "')");
    advance();
    auto e = std::make_unique<Expr>();
    e->span = previous().span;
    e->node = NullLitExpr{};
    return e;
}

} // namespace transom
