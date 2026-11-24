#pragma once

#include "common/source.hpp"
#include "common/value.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

namespace transom {

// Native compile-time parsed RGBA color (no runtime hex string parsing needed)
struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    bool operator==(const Color& o) const {
        return r == o.r && g == o.g && b == o.b && a == o.a;
    }
    bool operator!=(const Color& o) const { return !(*this == o); }

    std::string toHex() const {
        auto clampByte = [](float f) -> int {
            int v = static_cast<int>(f * 255.0f + 0.5f);
            if (v < 0) return 0;
            if (v > 255) return 255;
            return v;
        };
        char buf[16];
        int alphaByte = clampByte(a);
        if (alphaByte < 255) {
            std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x",
                          clampByte(r), clampByte(g), clampByte(b), alphaByte);
        } else {
            std::snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                          clampByte(r), clampByte(g), clampByte(b));
        }
        return std::string(buf);
    }

    static std::optional<Color> fromHex(const std::string& hex) {
        if (hex.empty() || hex[0] != '#') return std::nullopt;
        std::string s = hex.substr(1);
        if (s.size() == 3 || s.size() == 4) {
            std::string exp;
            for (char c : s) { exp.push_back(c); exp.push_back(c); }
            s = exp;
        }
        if (s.size() != 6 && s.size() != 8) return std::nullopt;
        char* end = nullptr;
        unsigned long long val = std::strtoull(s.c_str(), &end, 16);
        if (!end || *end != '\0') return std::nullopt;

        Color c;
        if (s.size() == 6) {
            c.r = ((val >> 16) & 0xFF) / 255.0f;
            c.g = ((val >> 8) & 0xFF) / 255.0f;
            c.b = (val & 0xFF) / 255.0f;
            c.a = 1.0f;
        } else {
            c.r = ((val >> 24) & 0xFF) / 255.0f;
            c.g = ((val >> 16) & 0xFF) / 255.0f;
            c.b = ((val >> 8) & 0xFF) / 255.0f;
            c.a = (val & 0xFF) / 255.0f;
        }
        return c;
    }
};

enum class StyleValKind : uint8_t {
    Int = 0,
    Float = 1,
    String = 2,
    Color = 3,
    TokenRef = 4
};

enum class InteractiveState : uint8_t {
    Normal = 0,
    Hover = 1,
    Pressed = 2,
    Disabled = 3
};

struct StyleValue {
    StyleValKind kind = StyleValKind::Int;
    int64_t intVal = 0;
    double floatVal = 0.0;
    std::string strVal; // String value or TokenRef name (without '$')
    Color colorVal;
    Span span;

    static StyleValue makeInt(int64_t v, Span sp = {}) {
        StyleValue sv; sv.kind = StyleValKind::Int; sv.intVal = v; sv.span = sp; return sv;
    }
    static StyleValue makeFloat(double v, Span sp = {}) {
        StyleValue sv; sv.kind = StyleValKind::Float; sv.floatVal = v; sv.span = sp; return sv;
    }
    static StyleValue makeString(std::string v, Span sp = {}) {
        StyleValue sv; sv.kind = StyleValKind::String; sv.strVal = std::move(v); sv.span = sp; return sv;
    }
    static StyleValue makeColor(Color c, Span sp = {}) {
        StyleValue sv; sv.kind = StyleValKind::Color; sv.colorVal = c; sv.span = sp; return sv;
    }
    static StyleValue makeTokenRef(std::string name, Span sp = {}) {
        StyleValue sv; sv.kind = StyleValKind::TokenRef; sv.strVal = std::move(name); sv.span = sp; return sv;
    }

    Value toRuntimeValue() const {
        switch (kind) {
            case StyleValKind::Int: return Value(intVal);
            case StyleValKind::Float: return Value(floatVal);
            case StyleValKind::String: return Value(strVal);
            case StyleValKind::Color: return Value(colorVal.toHex());
            case StyleValKind::TokenRef: return Value(strVal);
        }
        return Value(Null{});
    }
};

} // namespace transom
