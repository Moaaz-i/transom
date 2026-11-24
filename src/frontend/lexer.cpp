#include "frontend/lexer.hpp"

#include <cctype>
#include <unordered_map>

namespace transom {

const char* tokenKindName(TokenKind kind) {
    switch (kind) {
        case TokenKind::Int: return "Int";
        case TokenKind::Float: return "Float";
        case TokenKind::String: return "String";
        case TokenKind::StringPart: return "StringPart";
        case TokenKind::InterpStart: return "InterpStart";
        case TokenKind::InterpEnd: return "InterpEnd";
        case TokenKind::StringEnd: return "StringEnd";
        case TokenKind::Ident: return "Ident";
        case TokenKind::KwImport: return "import";
        case TokenKind::KwUse: return "use";
        case TokenKind::KwStyle: return "style";
        case TokenKind::KwPlugin: return "plugin";
        case TokenKind::KwExport: return "export";
        case TokenKind::KwApp: return "app";
        case TokenKind::KwComponent: return "component";
        case TokenKind::KwStruct: return "struct";
        case TokenKind::KwEnum: return "enum";
        case TokenKind::KwMatch: return "match";
        case TokenKind::KwPersist: return "persist";
        case TokenKind::KwState: return "state";
        case TokenKind::KwDerived: return "derived";
        case TokenKind::KwView: return "view";
        case TokenKind::KwSlot: return "slot";
        case TokenKind::KwEvery: return "every";
        case TokenKind::KwFn: return "fn";
        case TokenKind::KwLet: return "let";
        case TokenKind::KwIf: return "if";
        case TokenKind::KwElse: return "else";
        case TokenKind::KwWhile: return "while";
        case TokenKind::KwFor: return "for";
        case TokenKind::KwIn: return "in";
        case TokenKind::KwBreak: return "break";
        case TokenKind::KwContinue: return "continue";
        case TokenKind::KwConst: return "const";
        case TokenKind::KwReturn: return "return";
        case TokenKind::KwTrue: return "true";
        case TokenKind::KwFalse: return "false";
        case TokenKind::KwNull: return "null";
        case TokenKind::KwAnd: return "and";
        case TokenKind::KwOr: return "or";
        case TokenKind::KwNot: return "not";
        case TokenKind::Plus: return "+";
        case TokenKind::Minus: return "-";
        case TokenKind::Star: return "*";
        case TokenKind::Slash: return "/";
        case TokenKind::Percent: return "%";
        case TokenKind::Equal: return "=";
        case TokenKind::PlusEqual: return "+=";
        case TokenKind::MinusEqual: return "-=";
        case TokenKind::StarEqual: return "*=";
        case TokenKind::SlashEqual: return "/=";
        case TokenKind::EqualEqual: return "==";
        case TokenKind::BangEqual: return "!=";
        case TokenKind::Less: return "<";
        case TokenKind::LessEqual: return "<=";
        case TokenKind::Greater: return ">";
        case TokenKind::GreaterEqual: return ">=";
        case TokenKind::Bang: return "!";
        case TokenKind::AmpAmp: return "&&";
        case TokenKind::PipePipe: return "||";
        case TokenKind::Arrow: return "->";
        case TokenKind::FatArrow: return "=>";
        case TokenKind::PipeGreater: return "|>";
        case TokenKind::QuestionQuestion: return "??";
        case TokenKind::LParen: return "(";
        case TokenKind::RParen: return ")";
        case TokenKind::LBrace: return "{";
        case TokenKind::RBrace: return "}";
        case TokenKind::LBracket: return "[";
        case TokenKind::RBracket: return "]";
        case TokenKind::Comma: return ",";
        case TokenKind::Colon: return ":";
        case TokenKind::Dot: return ".";
        case TokenKind::Semicolon: return ";";
        case TokenKind::Newline: return "newline";
        case TokenKind::Eof: return "EOF";
        case TokenKind::Error: return "Error";
    }
    return "?";
}

Lexer::Lexer(std::string source, DiagnosticEngine& diag)
    : src_(std::move(source)), diag_(diag) {}

char Lexer::advance() {
    char c = src_[pos_++];
    if (c == '\n') {
        line_++;
        col_ = 1;
    } else {
        col_++;
    }
    return c;
}

bool Lexer::match(char expected) {
    if (isAtEnd() || src_[pos_] != expected) return false;
    advance();
    return true;
}

void Lexer::skipSpacesAndComments() {
    while (!isAtEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r') {
            advance();
        } else if (c == '/' && peek(1) == '/') {
            while (!isAtEnd() && peek() != '\n') advance();
        } else if (c == '/' && peek(1) == '*') {
            Span start{line_, col_, static_cast<uint32_t>(pos_), 2};
            advance();
            advance();
            bool closed = false;
            while (!isAtEnd()) {
                if (peek() == '*' && peek(1) == '/') {
                    advance();
                    advance();
                    closed = true;
                    break;
                }
                advance();
            }
            if (!closed) {
                diag_.error(start, "Unterminated block comment");
            }
        } else {
            break;
        }
    }
}

char Lexer::scanEscape(Span span) {
    if (isAtEnd()) {
        diag_.error(span, "Unterminated escape sequence");
        return '\0';
    }
    char c = advance();
    switch (c) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case '\\': return '\\';
        case '"': return '"';
        case '{': return '{';
        case '}': return '}';
        default:
            diag_.error(span, std::string("Unknown escape sequence '\\") + c + "'");
            return c;
    }
}

void Lexer::scanString(std::vector<Token>& out, bool isContinuation) {
    Span start{line_, col_, static_cast<uint32_t>(pos_), 1};
    std::string buf;

    while (!isAtEnd()) {
        char c = peek();
        if (c == '"') {
            advance();
            Token tok;
            tok.kind = isContinuation ? TokenKind::StringEnd : TokenKind::String;
            tok.lexeme = std::move(buf);
            tok.span = start;
            tok.span.length = static_cast<uint32_t>(pos_ - start.offset);
            out.push_back(std::move(tok));
            return;
        }
        if (c == '\n') {
            diag_.error(start, "Unterminated string literal");
            return;
        }
        if (c == '\\') {
            Span escSpan{line_, col_, static_cast<uint32_t>(pos_), 2};
            advance();
            buf.push_back(scanEscape(escSpan));
            continue;
        }
        if (c == '{') {
            Token part;
            part.kind = TokenKind::StringPart;
            part.lexeme = std::move(buf);
            part.span = start;
            part.span.length = static_cast<uint32_t>(pos_ - start.offset);
            out.push_back(std::move(part));

            Span braceSpan{line_, col_, static_cast<uint32_t>(pos_), 1};
            advance(); // consume '{'
            out.push_back({TokenKind::InterpStart, "{", braceSpan});
            interpBraceDepth_.push_back(0);
            return;
        }
        buf.push_back(advance());
    }
    diag_.error(start, "Unterminated string literal");
}

Token Lexer::scanNumber() {
    Span start{line_, col_, static_cast<uint32_t>(pos_), 0};
    bool isFloat = false;
    while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
        advance();
    }
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
        isFloat = true;
        advance(); // '.'
        while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
    }
    start.length = static_cast<uint32_t>(pos_ - start.offset);
    std::string lex = src_.substr(start.offset, start.length);
    return {isFloat ? TokenKind::Float : TokenKind::Int, std::move(lex), start};
}

Token Lexer::scanIdentOrKeyword() {
    static const std::unordered_map<std::string, TokenKind> kKeywords = {
        {"import", TokenKind::KwImport},
        {"use", TokenKind::KwUse},
        {"style", TokenKind::KwStyle},
        {"plugin", TokenKind::KwPlugin},
        {"export", TokenKind::KwExport},
        {"app", TokenKind::KwApp},
        {"component", TokenKind::KwComponent},
        {"struct", TokenKind::KwStruct},
        {"enum", TokenKind::KwEnum},
        {"match", TokenKind::KwMatch},
        {"persist", TokenKind::KwPersist},
        {"state", TokenKind::KwState},
        {"derived", TokenKind::KwDerived},
        {"view", TokenKind::KwView},
        {"slot", TokenKind::KwSlot},
        {"every", TokenKind::KwEvery},
        {"fn", TokenKind::KwFn},
        {"let", TokenKind::KwLet},
        {"if", TokenKind::KwIf},
        {"else", TokenKind::KwElse},
        {"while", TokenKind::KwWhile},
        {"for", TokenKind::KwFor},
        {"in", TokenKind::KwIn},
        {"break", TokenKind::KwBreak},
        {"continue", TokenKind::KwContinue},
        {"const", TokenKind::KwConst},
        {"return", TokenKind::KwReturn},
        {"true", TokenKind::KwTrue},
        {"false", TokenKind::KwFalse},
        {"null", TokenKind::KwNull},
        {"and", TokenKind::KwAnd},
        {"or", TokenKind::KwOr},
        {"not", TokenKind::KwNot},
    };

    Span start{line_, col_, static_cast<uint32_t>(pos_), 0};
    while (!isAtEnd()) {
        unsigned char c = static_cast<unsigned char>(peek());
        if (std::isalnum(c) || c == '_') {
            advance();
        } else {
            break;
        }
    }
    start.length = static_cast<uint32_t>(pos_ - start.offset);
    std::string lex = src_.substr(start.offset, start.length);
    auto it = kKeywords.find(lex);
    TokenKind kind = (it != kKeywords.end()) ? it->second : TokenKind::Ident;
    return {kind, std::move(lex), start};
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;

    while (!isAtEnd()) {
        skipSpacesAndComments();
        if (isAtEnd()) break;

        Span span{line_, col_, static_cast<uint32_t>(pos_), 1};
        char c = peek();

        if (c == '\n') {
            advance();
            if (parenDepth_ == 0 &&
                (tokens.empty() || tokens.back().kind != TokenKind::Newline)) {
                tokens.push_back({TokenKind::Newline, "\\n", span});
            }
            continue;
        }

        if (c == '"') {
            advance();
            scanString(tokens, false);
            continue;
        }

        if (std::isdigit(static_cast<unsigned char>(c))) {
            tokens.push_back(scanNumber());
            continue;
        }

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            tokens.push_back(scanIdentOrKeyword());
            continue;
        }

        advance();
        auto emit = [&](TokenKind k, const char* text) {
            Span s = span;
            s.length = static_cast<uint32_t>(pos_ - span.offset);
            tokens.push_back({k, text, s});
        };

        switch (c) {
            case '(':
                parenDepth_++;
                emit(TokenKind::LParen, "(");
                break;
            case ')':
                if (parenDepth_ > 0) parenDepth_--;
                emit(TokenKind::RParen, ")");
                break;
            case '[':
                parenDepth_++;
                emit(TokenKind::LBracket, "[");
                break;
            case ']':
                if (parenDepth_ > 0) parenDepth_--;
                emit(TokenKind::RBracket, "]");
                break;
            case '{':
                if (!interpBraceDepth_.empty()) interpBraceDepth_.back()++;
                emit(TokenKind::LBrace, "{");
                break;
            case '}':
                if (!interpBraceDepth_.empty()) {
                    if (interpBraceDepth_.back() == 0) {
                        interpBraceDepth_.pop_back();
                        emit(TokenKind::InterpEnd, "}");
                        scanString(tokens, true);
                        break;
                    }
                    interpBraceDepth_.back()--;
                }
                emit(TokenKind::RBrace, "}");
                break;
            case ',': emit(TokenKind::Comma, ","); break;
            case ':': emit(TokenKind::Colon, ":"); break;
            case '.': emit(TokenKind::Dot, "."); break;
            case ';': emit(TokenKind::Semicolon, ";"); break;
            case '%': emit(TokenKind::Percent, "%"); break;
            case '+':
                if (match('=')) emit(TokenKind::PlusEqual, "+=");
                else emit(TokenKind::Plus, "+");
                break;
            case '-':
                if (match('=')) emit(TokenKind::MinusEqual, "-=");
                else if (match('>')) emit(TokenKind::Arrow, "->");
                else emit(TokenKind::Minus, "-");
                break;
            case '*':
                if (match('=')) emit(TokenKind::StarEqual, "*=");
                else emit(TokenKind::Star, "*");
                break;
            case '/':
                if (match('=')) emit(TokenKind::SlashEqual, "/=");
                else emit(TokenKind::Slash, "/");
                break;
            case '=':
                if (match('=')) emit(TokenKind::EqualEqual, "==");
                else if (match('>')) emit(TokenKind::FatArrow, "=>");
                else emit(TokenKind::Equal, "=");
                break;
            case '!':
                if (match('=')) emit(TokenKind::BangEqual, "!=");
                else emit(TokenKind::Bang, "!");
                break;
            case '<':
                if (match('=')) emit(TokenKind::LessEqual, "<=");
                else emit(TokenKind::Less, "<");
                break;
            case '>':
                if (match('=')) emit(TokenKind::GreaterEqual, ">=");
                else emit(TokenKind::Greater, ">");
                break;
            case '&':
                if (match('&')) emit(TokenKind::AmpAmp, "&&");
                else diag_.error(span, "Unexpected '&'; use '&&' or 'and'");
                break;
            case '|':
                if (match('|')) emit(TokenKind::PipePipe, "||");
                else if (match('>')) emit(TokenKind::PipeGreater, "|>");
                else diag_.error(span, "Unexpected '|'; use '||', '|>', or 'or'");
                break;
            case '?':
                if (match('?')) emit(TokenKind::QuestionQuestion, "?\?");
                else diag_.error(span, "Unexpected '?'; use '?\?'");
                break;
            default:
                diag_.error(span, std::string("Unexpected character '") + c + "'");
                break;
        }
    }

    tokens.push_back({TokenKind::Eof, "", {line_, col_, static_cast<uint32_t>(pos_), 0}});
    return tokens;
}

} // namespace transom
