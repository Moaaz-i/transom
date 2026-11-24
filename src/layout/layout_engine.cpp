#include "layout/layout_engine.hpp"

namespace transom::layout {

Align parseAlign(const std::string& s, Align fallback) {
    if (s == "start" || s == "leading" || s == "top" || s == "left") return Align::Start;
    if (s == "center") return Align::Center;
    if (s == "end" || s == "trailing" || s == "bottom" || s == "right") return Align::End;
    if (s == "stretch") return Align::Stretch;
    return fallback;
}

Justify parseJustify(const std::string& s, Justify fallback) {
    if (s == "start") return Justify::Start;
    if (s == "center") return Justify::Center;
    if (s == "end") return Justify::End;
    if (s == "between" || s == "space-between") return Justify::SpaceBetween;
    if (s == "around" || s == "space-around") return Justify::SpaceAround;
    return fallback;
}

namespace {

float clampDim(float val, float minV, float maxV) {
    return std::max(minV, std::min(maxV, val));
}

float snapPixel(float val, float dpiScale) {
    if (dpiScale <= 0.0f) return val;
    return std::round(val * dpiScale) / dpiScale;
}

} // namespace

void measure(LayoutNode& node, float availableWidth, float availableHeight) {
    const float pad2 = node.style.padding * 2.0f;
    float innerAvailW = std::max(0.0f, (node.style.width >= 0.0f ? node.style.width : availableWidth) - pad2);
    float innerAvailH = std::max(0.0f, (node.style.height >= 0.0f ? node.style.height : availableHeight) - pad2);

    for (auto& ch : node.children) {
        measure(ch, innerAvailW, innerAvailH);
    }

    float contentW = node.style.intrinsicWidth;
    float contentH = node.style.intrinsicHeight;

    switch (node.kind) {
        case LayoutKind::Leaf:
        case LayoutKind::Spacer:
            break;

        case LayoutKind::VStack:
        case LayoutKind::Card:
        case LayoutKind::ScrollView: {
            float sumH = 0.0f;
            float maxW = 0.0f;
            for (size_t i = 0; i < node.children.size(); ++i) {
                sumH += node.children[i].measuredHeight;
                if (i + 1 < node.children.size()) sumH += node.style.gap;
                maxW = std::max(maxW, node.children[i].measuredWidth);
            }
            contentW = std::max(contentW, maxW);
            contentH = std::max(contentH, sumH);
            break;
        }

        case LayoutKind::HStack: {
            float sumW = 0.0f;
            float maxH = 0.0f;
            for (size_t i = 0; i < node.children.size(); ++i) {
                sumW += node.children[i].measuredWidth;
                if (i + 1 < node.children.size()) sumW += node.style.gap;
                maxH = std::max(maxH, node.children[i].measuredHeight);
            }
            contentW = std::max(contentW, sumW);
            contentH = std::max(contentH, maxH);
            break;
        }

        case LayoutKind::ZStack: {
            float maxW = 0.0f;
            float maxH = 0.0f;
            for (const auto& ch : node.children) {
                maxW = std::max(maxW, ch.measuredWidth);
                maxH = std::max(maxH, ch.measuredHeight);
            }
            contentW = std::max(contentW, maxW);
            contentH = std::max(contentH, maxH);
            break;
        }

        case LayoutKind::Grid: {
            int cols = std::max(1, node.style.columns);
            size_t n = node.children.size();
            size_t rows = (n + static_cast<size_t>(cols) - 1) / static_cast<size_t>(cols);
            float maxCellW = 0.0f;
            float totalH = 0.0f;
            for (size_t r = 0; r < rows; ++r) {
                float rowH = 0.0f;
                for (int c = 0; c < cols; ++c) {
                    size_t idx = r * static_cast<size_t>(cols) + static_cast<size_t>(c);
                    if (idx >= n) break;
                    maxCellW = std::max(maxCellW, node.children[idx].measuredWidth);
                    rowH = std::max(rowH, node.children[idx].measuredHeight);
                }
                totalH += rowH;
                if (r + 1 < rows) totalH += node.style.gap;
            }
            float totalW = maxCellW * static_cast<float>(cols) +
                           node.style.gap * static_cast<float>(std::max(0, cols - 1));
            contentW = std::max(contentW, totalW);
            contentH = std::max(contentH, totalH);
            break;
        }
    }

    float rawW = (node.style.width >= 0.0f) ? node.style.width : (contentW + pad2);
    float rawH = (node.style.height >= 0.0f) ? node.style.height : (contentH + pad2);

    node.measuredWidth = clampDim(rawW, node.style.minWidth, node.style.maxWidth);
    node.measuredHeight = clampDim(rawH, node.style.minHeight, node.style.maxHeight);
}

void arrange(LayoutNode& node, float x, float y, float width, float height) {
    float dpi = node.style.dpiScale;
    float finalW = clampDim(width, node.style.minWidth, node.style.maxWidth);
    float finalH = clampDim(height, node.style.minHeight, node.style.maxHeight);

    node.frame = {snapPixel(x, dpi), snapPixel(y, dpi), snapPixel(finalW, dpi), snapPixel(finalH, dpi)};

    if (node.children.empty()) return;

    const float pad = node.style.padding;
    const float innerX = node.frame.x + pad;
    const float innerY = node.frame.y + pad;
    const float innerW = std::max(0.0f, node.frame.width - 2.0f * pad);
    const float innerH = std::max(0.0f, node.frame.height - 2.0f * pad);
    const float gap = node.style.gap;
    const size_t n = node.children.size();

    if (node.kind == LayoutKind::ZStack) {
        for (auto& ch : node.children) {
            float cw = (node.style.align == Align::Stretch) ? innerW : std::min(innerW, ch.measuredWidth);
            float chH = (node.style.align == Align::Stretch) ? innerH : std::min(innerH, ch.measuredHeight);
            float cx = innerX;
            float cy = innerY;
            if (node.style.align == Align::Center) {
                cx = innerX + (innerW - cw) * 0.5f;
                cy = innerY + (innerH - chH) * 0.5f;
            } else if (node.style.align == Align::End) {
                cx = innerX + (innerW - cw);
                cy = innerY + (innerH - chH);
            }
            arrange(ch, cx, cy, cw, chH);
        }
        return;
    }

    if (node.kind == LayoutKind::Grid) {
        int cols = std::max(1, node.style.columns);
        size_t rows = (n + static_cast<size_t>(cols) - 1) / static_cast<size_t>(cols);
        float cellW = std::max(0.0f, (innerW - gap * static_cast<float>(std::max(0, cols - 1))) / static_cast<float>(cols));
        float curY = innerY;
        for (size_t r = 0; r < rows; ++r) {
            float rowH = 0.0f;
            for (int c = 0; c < cols; ++c) {
                size_t idx = r * static_cast<size_t>(cols) + static_cast<size_t>(c);
                if (idx >= n) break;
                rowH = std::max(rowH, node.children[idx].measuredHeight);
            }
            for (int c = 0; c < cols; ++c) {
                size_t idx = r * static_cast<size_t>(cols) + static_cast<size_t>(c);
                if (idx >= n) break;
                int visualCol = node.style.rtl ? (cols - 1 - c) : c;
                float cx = innerX + static_cast<float>(visualCol) * (cellW + gap);
                arrange(node.children[idx], cx, curY, cellW, rowH);
            }
            curY += rowH + gap;
        }
        return;
    }

    // Flex along main axis: VStack/Card/ScrollView (vertical) or HStack (horizontal)
    const bool isRow = (node.kind == LayoutKind::HStack);
    const float mainAvail = isRow ? innerW : innerH;
    const float crossAvail = isRow ? innerH : innerW;
    const float totalGaps = gap * static_cast<float>(n > 0 ? n - 1 : 0);

    float usedMain = 0.0f;
    float totalGrow = 0.0f;
    float totalShrink = 0.0f;
    for (const auto& ch : node.children) {
        float base = isRow ? ch.measuredWidth : ch.measuredHeight;
        usedMain += base;
        float g = ch.style.grow;
        if (ch.kind == LayoutKind::Spacer && g <= 0.0f &&
            (isRow ? ch.style.width < 0.0f : ch.style.height < 0.0f)) {
            g = 1.0f;
        }
        totalGrow += g;
        totalShrink += ch.style.shrink;
    }

    float freeSpace = mainAvail - usedMain - totalGaps;
    std::vector<float> childMain(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const auto& ch = node.children[i];
        float base = isRow ? ch.measuredWidth : ch.measuredHeight;
        float g = ch.style.grow;
        if (ch.kind == LayoutKind::Spacer && g <= 0.0f &&
            (isRow ? ch.style.width < 0.0f : ch.style.height < 0.0f)) {
            g = 1.0f;
        }
        if (freeSpace > 0.0f && totalGrow > 0.0f) {
            base += freeSpace * (g / totalGrow);
        } else if (freeSpace < 0.0f && totalShrink > 0.0f && !node.style.noShrink) {
            base = std::max(0.0f, base + freeSpace * (ch.style.shrink / totalShrink));
        }
        float minM = isRow ? ch.style.minWidth : ch.style.minHeight;
        float maxM = isRow ? ch.style.maxWidth : ch.style.maxHeight;
        childMain[i] = clampDim(base, minM, maxM);
    }

    float actualUsed = totalGaps;
    for (float m : childMain) actualUsed += m;
    float remaining = std::max(0.0f, mainAvail - actualUsed);

    float offset = 0.0f;
    float extraGap = 0.0f;
    switch (node.style.justify) {
        case Justify::Start:
            break;
        case Justify::Center:
            offset = remaining * 0.5f;
            break;
        case Justify::End:
            offset = remaining;
            break;
        case Justify::SpaceBetween:
            if (n > 1) extraGap = remaining / static_cast<float>(n - 1);
            break;
        case Justify::SpaceAround:
            extraGap = remaining / static_cast<float>(n);
            offset = extraGap * 0.5f;
            break;
    }

    float cursor = offset;
    for (size_t i = 0; i < n; ++i) {
        auto& ch = node.children[i];
        float mSize = childMain[i];
        float cNat = isRow ? ch.measuredHeight : ch.measuredWidth;
        const bool crossUnspecified = isRow ? (ch.style.height < 0.0f) : (ch.style.width < 0.0f);
        const bool stretch = node.style.align == Align::Stretch || (ch.style.fillCross && crossUnspecified);
        float cSize = stretch ? crossAvail : std::min(crossAvail, cNat);

        float crossPos = 0.0f;
        if (!stretch) {
            if (node.style.align == Align::Center) crossPos = (crossAvail - cSize) * 0.5f;
            else if (node.style.align == Align::End) crossPos = crossAvail - cSize;
        }

        if (isRow) {
            float cx = node.style.rtl ? (innerX + innerW - cursor - mSize) : (innerX + cursor);
            float cy = innerY + crossPos;
            arrange(ch, cx, cy, mSize, cSize);
        } else {
            float cx = innerX + (node.style.rtl && node.style.align == Align::Start
                                     ? (innerW - cSize)
                                     : crossPos);
            float cy = innerY + cursor;
            arrange(ch, cx, cy, cSize, mSize);
        }
        cursor += mSize + gap + extraGap;
    }
}

void computeLayout(LayoutNode& root, float viewportWidth, float viewportHeight) {
    measure(root, viewportWidth, viewportHeight);
    arrange(root, 0.0f, 0.0f, viewportWidth, viewportHeight);
}

} // namespace transom::layout
