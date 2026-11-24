#pragma once

#include "common/source.hpp"
#include "style/stylesheet.hpp"

namespace transom {

// Validates .tstyle AST (tokens, theme override contracts, property types,
// single inheritance & circular inheritance detection) and compiles it into
// a flat, runtime-ready CompiledStyleSheet.
class StyleChecker {
public:
    explicit StyleChecker(DiagnosticEngine& diag);

    CompiledStyleSheet checkAndCompile(const StyleSheetAST& ast);

private:
    DiagnosticEngine& diag_;

    StyleValKind expectedPropType(const std::string& propName) const;
    bool validateProp(const StylePropDecl& prop,
                      const std::unordered_map<std::string, StyleValue>& tokens);
};

} // namespace transom
