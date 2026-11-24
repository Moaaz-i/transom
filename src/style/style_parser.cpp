#include "style/style_parser.hpp"

#include <cctype>

namespace transom {

StyleParser::StyleParser(std::string source, DiagnosticEngine& diag)
    : src_(std::move(source)), diag_(diag) {}

char StyleParser::advance() {
    char c = src_[pos_++];
    if (c == '\n') { line_++; col_ = 1; }
    else { col_++; }
    return c;
}

void StyleParser::skipWhitespaceAndComments() {
    while (!isAtEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ';') {
            advance();
        } else if (c == '/' && peek(1) == '/') {
            while (!isAtEnd() && peek() != '\n') advance();
        } else if (c == '/' && peek(1) == '*') {
            advance(); advance();
            while (!isAtEnd() && !(peek() == '*' && peek(1) == '/')) advance();
            if (!isAtEnd()) { advance(); advance(); }
        } else {
            break;
        }
    }
}

bool StyleParser::matchChar(char expected) {
    skipWhitespaceAndComments();
    if (!isAtEnd() && peek() == expected) {
        advance();
        return true;
    }
    return false;
}

std::string StyleParser::scanIdent() {
    skipWhitespaceAndComments();
    std::string out;
    while (!isAtEnd()) {
        unsigned char c = static_cast<unsigned char>(peek());
        if (std::isalnum(c) || c == '_' || c == '-') {
            out.push_back(advance());
        } else {
            break;
        }
    }
    return out;
}

std::string StyleParser::scanString() {
    skipWhitespaceAndComments();
    Span sp = currentSpan();
    if (isAtEnd() || peek() != '"') {
        diag_.error(sp, "Expected string literal in .tstyle");
        return "";
    }
    advance(); // '"'
    std::string out;
    while (!isAtEnd() && peek() != '"') {
        if (peek() == '\\' && pos_ + 1 < src_.size()) {
            advance();
            out.push_back(advance());
        } else {
            out.push_back(advance());
        }
    }
    if (isAtEnd()) diag_.error(sp, "Unterminated string literal in .tstyle");
    else advance(); // '"'
    return out;
}

StyleValue StyleParser::scanValue() {
    skipWhitespaceAndComments();
    Span sp = currentSpan();
    if (isAtEnd()) {
        diag_.error(sp, "Expected style value");
        return StyleValue::makeInt(0, sp);
    }

    char c = peek();
    if (c == '$') {
        advance();
        std::string tokName = scanIdent();
        if (tokName.empty()) {
            diag_.error(sp, "Expected token name after '$'");
        }
        return StyleValue::makeTokenRef(std::move(tokName), sp);
    }

    if (c == '"') {
        std::string s = scanString();
        if (!s.empty() && s[0] == '#') {
            auto col = Color::fromHex(s);
            if (!col) {
                diag_.error(sp, "Invalid hex color '" + s + "' (expected #RGB, #RRGGBB, or #RRGGBBAA)");
                return StyleValue::makeColor(Color{}, sp);
            }
            return StyleValue::makeColor(*col, sp);
        }
        return StyleValue::makeString(std::move(s), sp);
    }

    if (c == '#') {
        std::string hex;
        hex.push_back(advance());
        while (!isAtEnd() && std::isxdigit(static_cast<unsigned char>(peek()))) {
            hex.push_back(advance());
        }
        auto col = Color::fromHex(hex);
        if (!col) {
            diag_.error(sp, "Invalid hex color '" + hex + "'");
            return StyleValue::makeColor(Color{}, sp);
        }
        return StyleValue::makeColor(*col, sp);
    }

    if (std::isdigit(static_cast<unsigned char>(c)) || (c == '-' && std::isdigit(static_cast<unsigned char>(peek(1))))) {
        std::string num;
        if (c == '-') num.push_back(advance());
        bool isFloat = false;
        while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
            num.push_back(advance());
        }
        if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
            isFloat = true;
            num.push_back(advance());
            while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
                num.push_back(advance());
            }
        }
        // Optional native UI units: dp, pt, or ms (for transition)
        if ((peek() == 'd' && peek(1) == 'p') || (peek() == 'p' && peek(1) == 't') ||
            (peek() == 'm' && peek(1) == 's')) {
            advance();
            advance();
        }
        if (isFloat) return StyleValue::makeFloat(std::stod(num), sp);
        return StyleValue::makeInt(std::stoll(num), sp);
    }

    std::string word = scanIdent();
    if (!word.empty()) {
        return StyleValue::makeString(std::move(word), sp);
    }

    diag_.error(sp, std::string("Unexpected character '") + c + "' in style value");
    advance();
    return StyleValue::makeInt(0, sp);
}

StylePropDecl StyleParser::parsePropDecl() {
    skipWhitespaceAndComments();
    Span sp = currentSpan();
    std::string name = scanIdent();
    if (!matchChar(':')) {
        diag_.error(currentSpan(), "Expected ':' after style property '" + name + "'");
    }
    StyleValue val = scanValue();
    return {std::move(name), std::move(val), sp};
}

std::vector<StylePropDecl> StyleParser::parsePropBlock() {
    std::vector<StylePropDecl> props;
    if (!matchChar('{')) {
        diag_.error(currentSpan(), "Expected '{'");
        return props;
    }
    skipWhitespaceAndComments();
    while (!isAtEnd() && peek() != '}') {
        size_t before = pos_;
        props.push_back(parsePropDecl());
        if (pos_ == before) advance();
        skipWhitespaceAndComments();
    }
    if (!matchChar('}')) {
        diag_.error(currentSpan(), "Expected '}'");
    }
    return props;
}

StyleRuleDecl StyleParser::parseRule(bool isWidgetDefault, std::string name, Span sp) {
    StyleRuleDecl rule;
    rule.isWidgetDefault = isWidgetDefault;
    rule.name = std::move(name);
    rule.span = sp;

    if (!isWidgetDefault && matchChar(':')) {
        rule.parent = scanIdent();
        if (rule.parent.empty()) {
            diag_.error(currentSpan(), "Expected parent style name after ':'");
        }
    }

    if (!matchChar('{')) {
        diag_.error(currentSpan(), "Expected '{' to open style rule '" + rule.name + "'");
        return rule;
    }

    skipWhitespaceAndComments();
    while (!isAtEnd() && peek() != '}') {
        size_t before = pos_;
        Span itemSp = currentSpan();
        std::string key = scanIdent();
        skipWhitespaceAndComments();
        if ((key == "hover" || key == "pressed" || key == "disabled") && peek() == '{') {
            StyleStateBlock sb;
            sb.span = itemSp;
            if (key == "hover") sb.state = InteractiveState::Hover;
            else if (key == "pressed") sb.state = InteractiveState::Pressed;
            else sb.state = InteractiveState::Disabled;
            sb.props = parsePropBlock();
            rule.states.push_back(std::move(sb));
        } else {
            if (!matchChar(':')) {
                diag_.error(currentSpan(), "Expected ':' after property '" + key + "'");
            }
            StyleValue val = scanValue();
            rule.props.push_back({std::move(key), std::move(val), itemSp});
        }
        if (pos_ == before) advance();
        skipWhitespaceAndComments();
    }

    if (!matchChar('}')) {
        diag_.error(currentSpan(), "Expected '}' to close style rule '" + rule.name + "'");
    }
    return rule;
}

StyleSheetAST StyleParser::parse() {
    StyleSheetAST ast;
    skipWhitespaceAndComments();

    while (!isAtEnd()) {
        Span sp = currentSpan();
        std::string head = scanIdent();
        if (head.empty()) {
            diag_.error(sp, "Unexpected character in .tstyle file");
            advance();
            skipWhitespaceAndComments();
            continue;
        }

        if (head == "tokens") {
            auto props = parsePropBlock();
            for (auto& p : props) ast.tokens.push_back(std::move(p));
        } else if (head == "theme") {
            ThemeBlockDecl tb;
            tb.span = sp;
            tb.name = scanString();
            tb.overrides = parsePropBlock();
            ast.themes.push_back(std::move(tb));
        } else if (head == "style") {
            std::string cname = scanIdent();
            if (cname.empty()) {
                diag_.error(currentSpan(), "Expected style class name after 'style'");
            }
            ast.rules.push_back(parseRule(false, std::move(cname), sp));
        } else {
            // Widget default rule (e.g. `Card { ... }`, `Text { ... }`)
            ast.rules.push_back(parseRule(true, std::move(head), sp));
        }
        skipWhitespaceAndComments();
    }

    return ast;
}

} // namespace transom
