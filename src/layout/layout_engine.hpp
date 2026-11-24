#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace transom::layout {

enum class LayoutKind {
    Leaf,
    VStack,
    HStack,
    Grid,
    ZStack,
    Card,
    ScrollView,
    Spacer
};

enum class Align {
    Start,
    Center,
    End,
    Stretch
};

enum class Justify {
    Start,
    Center,
    End,
    SpaceBetween,
    SpaceAround
};

struct LayoutRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    float right() const { return x + width; }
    float bottom() const { return y + height; }
};

struct LayoutStyle {
    float width = -1.0f;      // < 0 means auto
    float height = -1.0f;     // < 0 means auto
    float minWidth = 0.0f;
    float maxWidth = 1e9f;
    float minHeight = 0.0f;
    float maxHeight = 1e9f;
    float grow = 0.0f;
    float shrink = 1.0f;
    float gap = 0.0f;
    float padding = 0.0f;
    int columns = 2;          // for Grid
    Align align = Align::Start;
    Justify justify = Justify::Start;
    bool rtl = false;
    bool fillCross = false;   // stretch on the parent's cross axis when that size is unspecified
    bool noShrink = false;    // never shrink along the main axis (e.g. ScrollView content)
    float dpiScale = 1.0f;

    // Platform-measured intrinsic content size (e.g. text bounds, icon size)
    float intrinsicWidth = 0.0f;
    float intrinsicHeight = 0.0f;
};

struct LayoutNode {
    uint64_t id = 0;
    LayoutKind kind = LayoutKind::Leaf;
    LayoutStyle style;
    std::vector<LayoutNode> children;

    // Computed in Measure phase
    float measuredWidth = 0.0f;
    float measuredHeight = 0.0f;

    // Computed in Arrange phase
    LayoutRect frame;
};

// Two-pass portable layout engine:
//   1. measure(): computes intrinsic/constrained sizes bottom-up.
//   2. arrange(): assigns final LayoutRect frames top-down (supports flex grow/shrink,
//                 cross-axis alignment, justify, Grid columns, ZStack overlay, RTL).
void measure(LayoutNode& node, float availableWidth, float availableHeight);
void arrange(LayoutNode& node, float x, float y, float width, float height);
void computeLayout(LayoutNode& root, float viewportWidth, float viewportHeight);

// Helpers for parsing string props into enums
Align parseAlign(const std::string& s, Align fallback = Align::Start);
Justify parseJustify(const std::string& s, Justify fallback = Justify::Start);

} // namespace transom::layout
