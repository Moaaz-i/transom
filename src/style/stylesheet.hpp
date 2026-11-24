#pragma once

#include "style/style_value.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace transom {

constexpr uint32_t kStyleSheetVersion = 1;

// ---------------------------------------------------------------------------
// Parsed .tstyle AST
// ---------------------------------------------------------------------------
struct StylePropDecl {
    std::string name;
    StyleValue value;
    Span span;
};

struct StyleStateBlock {
    InteractiveState state = InteractiveState::Normal;
    std::vector<StylePropDecl> props;
    Span span;
};

struct StyleRuleDecl {
    bool isWidgetDefault = false; // true for `Card { ... }`, false for `style foo { ... }`
    std::string name;
    std::string parent;           // optional single inheritance (`style a : b`)
    std::vector<StylePropDecl> props;
    std::vector<StyleStateBlock> states;
    Span span;
};

struct ThemeBlockDecl {
    std::string name;
    std::vector<StylePropDecl> overrides;
    Span span;
};

struct StyleSheetAST {
    std::vector<StylePropDecl> tokens;
    std::vector<ThemeBlockDecl> themes;
    std::vector<StyleRuleDecl> rules;
};

// ---------------------------------------------------------------------------
// CompiledStyleSheet (inheritance already resolved at compile time!)
// ---------------------------------------------------------------------------
struct ResolvedStyleRule {
    std::unordered_map<std::string, StyleValue> normalProps;
    std::unordered_map<std::string, StyleValue> hoverProps;
    std::unordered_map<std::string, StyleValue> pressedProps;
    std::unordered_map<std::string, StyleValue> disabledProps;
};

struct CompiledStyleSheet {
    uint32_t version = kStyleSheetVersion;
    std::unordered_map<std::string, StyleValue> defaultTokens;
    std::unordered_map<std::string, std::unordered_map<std::string, StyleValue>> themes;
    std::unordered_map<std::string, ResolvedStyleRule> widgetDefaults;
    std::unordered_map<std::string, ResolvedStyleRule> classes;

    bool empty() const {
        return defaultTokens.empty() && themes.empty() &&
               widgetDefaults.empty() && classes.empty();
    }

    bool hasTheme(const std::string& name) const {
        return name == "default" || themes.count(name) > 0;
    }

    // Resolve a StyleValue under a specific theme (resolving $token refs)
    Value resolveValue(const StyleValue& sv, const std::string& themeName) const {
        if (sv.kind != StyleValKind::TokenRef) {
            return sv.toRuntimeValue();
        }
        if (themeName != "default") {
            auto tit = themes.find(themeName);
            if (tit != themes.end()) {
                auto oit = tit->second.find(sv.strVal);
                if (oit != tit->second.end()) {
                    return oit->second.toRuntimeValue();
                }
            }
        }
        auto dit = defaultTokens.find(sv.strVal);
        if (dit != defaultTokens.end()) {
            return dit->second.toRuntimeValue();
        }
        return Value(Null{});
    }

    std::string resolveWindowBg(const std::string& themeName) const {
        auto it = defaultTokens.find("bg");
        if (it == defaultTokens.end()) return "";
        return resolveValue(StyleValue::makeTokenRef("bg"), themeName).toString();
    }
};

} // namespace transom
