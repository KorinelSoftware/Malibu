#pragma once
// core/include/malibu/layout/layout_box.h
// A node in the layout tree (Task 12 / design §7.1). Mirrors the DOM tree but
// excludes display:none. Geometry is in CSS pixels with the document origin at
// the top-left.

#include <cstdint>
#include <string>
#include <vector>

#include "../types.h"
#include "../css/computed_style/computed_style.h"

namespace malibu::layout {

enum class BoxType : uint8_t { Block, Inline, InlineBlock, Flex, Text,
                               Table, TableRow, TableCell, TableRowGroup, ListItem, Grid };

// One wrapped line of a text box: a substring placed at its own position. The
// renderer draws one PaintText per fragment so long text wraps correctly.
struct TextFragment {
    std::u16string text;
    float x = 0, y = 0, w = 0;
};

struct LayoutBox {
    malibu::NodeHandle          node;
    const malibu::css::ComputedStyle* style = nullptr;  // null for anonymous/text
    BoxType                     type = BoxType::Block;
    std::u16string              text;                    // Text boxes only
    std::vector<TextFragment>   fragments;               // per-line pieces (Text boxes)

    LayoutBox*                  parent = nullptr;
    std::vector<LayoutBox*>     children;

    // Content-box geometry (document coordinates).
    float x = 0, y = 0;
    float width = 0, height = 0;

    // Resolved box-model edges (px): order top, right, bottom, left.
    float margin[4]  = {0, 0, 0, 0};
    float padding[4] = {0, 0, 0, 0};
    float border[4]  = {0, 0, 0, 0};

    bool is_dirty = true;

    [[nodiscard]] float margin_box_width()  const { return margin[3] + border[3] + padding[3] + width  + padding[1] + border[1] + margin[1]; }
    [[nodiscard]] float margin_box_height() const { return margin[0] + border[0] + padding[0] + height + padding[2] + border[2] + margin[2]; }
    [[nodiscard]] float border_box_width()  const { return border[3] + padding[3] + width  + padding[1] + border[1]; }
    [[nodiscard]] float border_box_height() const { return border[0] + padding[0] + height + padding[2] + border[2]; }
};

} // namespace malibu::layout
