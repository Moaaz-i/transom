#pragma once

#include "common/source.hpp"
#include "style/stylesheet.hpp"
#include <string>

namespace transom {

class StyleParser {
public:
    StyleParser(std::string source, DiagnosticEngine& diag);

    StyleSheetAST parse();

private:
    std::string src_;
    DiagnosticEngine& diag_;
    size_t pos_ = 0;
    uint32_t line_ = 1;
    uint32_t col_ = 1;

    bool isAtEnd() const { return pos_ >= src_.size(); }
    char peek(size_t off = 0) const {
        return (pos_ + off < src_.size()) ? src_[pos_ + off] : '\0';
    }
    char advance();
    void skipWhitespaceAndComments();
    bool matchChar(char expected);
    Span currentSpan() const { return {line_, col_, static_cast<uint32_t>(pos_), 1}; }

    std::string scanIdent();
    std::string scanString();
    StyleValue scanValue();
    StylePropDecl parsePropDecl();
    std::vector<StylePropDecl> parsePropBlock();
    StyleRuleDecl parseRule(bool isWidgetDefault, std::string name, Span sp);
};

} // namespace transom
