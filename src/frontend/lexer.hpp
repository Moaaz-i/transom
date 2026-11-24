#pragma once

#include "common/source.hpp"
#include "frontend/token.hpp"
#include <string>
#include <vector>

namespace transom {

class Lexer {
public:
    Lexer(std::string source, DiagnosticEngine& diag);

    std::vector<Token> tokenize();

private:
    std::string src_;
    DiagnosticEngine& diag_;
    size_t pos_ = 0;
    uint32_t line_ = 1;
    uint32_t col_ = 1;

    // Tracks nested string interpolations: each entry is the brace depth
    // inside that interpolation expression.
    std::vector<int> interpBraceDepth_;
    int parenDepth_ = 0;

    bool isAtEnd() const { return pos_ >= src_.size(); }
    char peek(size_t offset = 0) const {
        return (pos_ + offset < src_.size()) ? src_[pos_ + offset] : '\0';
    }
    char advance();
    bool match(char expected);

    void skipSpacesAndComments();
    void scanString(std::vector<Token>& out, bool isContinuation);
    Token scanNumber();
    Token scanIdentOrKeyword();
    char scanEscape(Span span);
};

} // namespace transom
