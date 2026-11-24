#include "style/style_checker.hpp"

#include <unordered_set>

namespace transom {

namespace {

const char* styleValKindName(StyleValKind k) {
    switch (k) {
        case StyleValKind::Int: return "Int";
        case StyleValKind::Float: return "Float";
        case StyleValKind::String: return "String";
        case StyleValKind::Color: return "Color";
        case StyleValKind::TokenRef: return "TokenRef";
    }
    return "?";
}

bool styleTypesCompatible(StyleValKind expected, StyleValKind actual) {
    if (expected == actual) return true;
    if (expected == StyleValKind::Float && actual == StyleValKind::Int) return true;
    return false;
}

} // namespace

StyleChecker::StyleChecker(DiagnosticEngine& diag) : diag_(diag) {}

StyleValKind StyleChecker::expectedPropType(const std::string& propName) const {
    if (propName == "bg" || propName == "color" || propName == "borderColor") {
        return StyleValKind::Color;
    }
    if (propName == "gap" || propName == "padding" || propName == "radius" ||
        propName == "size" || propName == "width" || propName == "height" ||
        propName == "minWidth" || propName == "maxWidth" ||
        propName == "minHeight" || propName == "maxHeight" ||
        propName == "borderWidth" || propName == "shadow" || propName == "transition") {
        return StyleValKind::Int;
    }
    if (propName == "opacity" || propName == "grow" || propName == "shrink") {
        return StyleValKind::Float;
    }
    if (propName == "weight" || propName == "align" || propName == "style") {
        return StyleValKind::String;
    }
    return StyleValKind::TokenRef; // sentinel for unknown property
}

bool StyleChecker::validateProp(const StylePropDecl& prop,
                                const std::unordered_map<std::string, StyleValue>& tokens) {
    StyleValKind expected = expectedPropType(prop.name);
    if (expected == StyleValKind::TokenRef) {
        diag_.error(prop.span, "Unknown style property '" + prop.name + "'");
        return false;
    }

    StyleValKind actual = prop.value.kind;
    if (actual == StyleValKind::TokenRef) {
        auto it = tokens.find(prop.value.strVal);
        if (it == tokens.end()) {
            diag_.error(prop.value.span,
                        "Undefined design token '$" + prop.value.strVal + "'");
            return false;
        }
        actual = it->second.kind;
    }

    if (!styleTypesCompatible(expected, actual)) {
        diag_.error(prop.span,
                    std::string("Style property '") + prop.name + "' expects " +
                        styleValKindName(expected) + ", got " + styleValKindName(actual));
        return false;
    }
    return true;
}

CompiledStyleSheet StyleChecker::checkAndCompile(const StyleSheetAST& ast) {
    CompiledStyleSheet out;

    // 1. Register default tokens
    for (const auto& t : ast.tokens) {
        if (t.value.kind == StyleValKind::TokenRef) {
            diag_.error(t.span, "Token '" + t.name + "' cannot reference another token");
            continue;
        }
        out.defaultTokens[t.name] = t.value;
    }

    // 2. Validate theme overrides: must override EXISTING tokens with COMPATIBLE types
    for (const auto& th : ast.themes) {
        if (th.name == "default") {
            diag_.error(th.span, "Theme name 'default' is reserved for base tokens");
            continue;
        }
        auto& overrides = out.themes[th.name];
        for (const auto& ov : th.overrides) {
            auto dit = out.defaultTokens.find(ov.name);
            if (dit == out.defaultTokens.end()) {
                diag_.error(ov.span,
                            "Theme '" + th.name + "' cannot define new token '" + ov.name +
                                "' (declare it in 'tokens { ... }' first)");
                continue;
            }
            if (!styleTypesCompatible(dit->second.kind, ov.value.kind)) {
                diag_.error(ov.span,
                            std::string("Theme '") + th.name + "' token '" + ov.name +
                                "' type mismatch: expected " + styleValKindName(dit->second.kind) +
                                ", got " + styleValKindName(ov.value.kind));
                continue;
            }
            overrides[ov.name] = ov.value;
        }
    }

    // 3. Index raw rules for inheritance resolution
    std::unordered_map<std::string, const StyleRuleDecl*> rawWidgets;
    std::unordered_map<std::string, const StyleRuleDecl*> rawClasses;

    for (const auto& r : ast.rules) {
        for (const auto& p : r.props) validateProp(p, out.defaultTokens);
        for (const auto& sb : r.states) {
            for (const auto& p : sb.props) validateProp(p, out.defaultTokens);
        }
        if (r.isWidgetDefault) rawWidgets[r.name] = &r;
        else rawClasses[r.name] = &r;
    }

    // Helper: merge one rule's props into a ResolvedStyleRule
    auto applyRule = [](ResolvedStyleRule& dst, const StyleRuleDecl& src) {
        for (const auto& p : src.props) dst.normalProps[p.name] = p.value;
        for (const auto& sb : src.states) {
            auto& map = (sb.state == InteractiveState::Hover)
                            ? dst.hoverProps
                            : (sb.state == InteractiveState::Pressed ? dst.pressedProps : dst.disabledProps);
            for (const auto& p : sb.props) map[p.name] = p.value;
        }
    };

    // 4. Compile widget defaults
    for (const auto& [wname, rptr] : rawWidgets) {
        ResolvedStyleRule resolved;
        applyRule(resolved, *rptr);
        out.widgetDefaults[wname] = std::move(resolved);
    }

    // 5. Compile classes with single-inheritance resolution + circular inheritance detection
    for (const auto& [cname, rptr] : rawClasses) {
        std::vector<const StyleRuleDecl*> chain;
        std::unordered_set<std::string> visited;
        const StyleRuleDecl* cur = rptr;
        bool cycle = false;

        while (cur) {
            if (!visited.insert(cur->name).second) {
                diag_.error(rptr->span, "Circular style inheritance detected involving '" + cur->name + "'");
                cycle = true;
                break;
            }
            chain.push_back(cur);
            if (cur->parent.empty()) break;

            auto cit = rawClasses.find(cur->parent);
            if (cit != rawClasses.end()) {
                cur = cit->second;
            } else {
                auto wit = rawWidgets.find(cur->parent);
                if (wit != rawWidgets.end()) {
                    chain.push_back(wit->second);
                    break;
                }
                diag_.error(cur->span,
                            "Style '" + cur->name + "' inherits from unknown style '" + cur->parent + "'");
                break;
            }
        }

        if (cycle) continue;

        ResolvedStyleRule resolved;
        // Apply from root ancestor down to child so child overrides parent deterministically
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            applyRule(resolved, **it);
        }
        out.classes[cname] = std::move(resolved);
    }

    return out;
}

} // namespace transom
