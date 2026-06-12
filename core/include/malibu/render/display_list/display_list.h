#pragma once
// core/include/malibu/render/display_list/display_list.h
// A flat, GPU-agnostic paint list built from the layout tree. Each item is
// self-contained (absolute transform / opacity / clip already accumulated) and
// the list is sorted by (z-index asc, document-order asc) so a back-to-front
// rasterizer or GPU backend can consume it directly (design §8.3).

#include <cstdint>
#include <string>
#include <vector>

#include "malibu/css/computed_style/computed_style.h"

namespace malibu::dom { class Document; }
namespace malibu::layout { struct LayoutBox; }

namespace malibu::render {

using malibu::css::Color;
using malibu::css::Transform2D;

struct ClipRect {
    bool  active = false;
    float x = 0, y = 0, w = 0, h = 0;
};

struct PaintRect {
    float x = 0, y = 0, w = 0, h = 0;        // border-box rect
    Color background = {0, 0, 0, 0};
    float border[4] = {0, 0, 0, 0};          // top,right,bottom,left widths
    Color border_color = {0, 0, 0, 255};
    Color border_colors[4] = {{0,0,0,255},{0,0,0,255},{0,0,0,255},{0,0,0,255}};  // per side
    float border_radius = 0;
    // linear-gradient background (overrides `background` when set)
    bool  gradient = false;
    float grad_angle = 180.0f;
    std::vector<malibu::css::ComputedStyle::GradientStop> grad_stops;
    // drop shadow (this item is the soft shadow pass, drawn behind the box)
    bool  shadow = false;
    float blur = 0.0f;
};

struct PaintText {
    float          x = 0, y = 0;
    std::u16string text;
    Color          color = {0, 0, 0, 255};
    float          font_size = 16.0f;
};

struct DisplayItem {
    enum class Kind : uint8_t { Rect, Text };
    Kind        kind = Kind::Rect;
    int32_t     z_index = 0;
    uint32_t    document_order = 0;
    // Lexicographic paint key. Each stacking context contributes
    // (stack-level, context document order), preventing descendants from
    // escaping their context while preserving order among equal levels.
    std::vector<int64_t> stacking_key;
    float       opacity = 1.0f;
    Transform2D transform;
    ClipRect    clip;
    PaintRect   rect;
    PaintText   text;
};

class DisplayList {
public:
    void add(DisplayItem item) { items_.push_back(std::move(item)); }
    void clear() { items_.clear(); }
    [[nodiscard]] const std::vector<DisplayItem>& items() const noexcept { return items_; }
    [[nodiscard]] std::vector<DisplayItem>&       items() noexcept { return items_; }

    // Stable-sorts by (z-index, document order) — back to front.
    void sort();

private:
    std::vector<DisplayItem> items_;
};

// Builds a display list from a laid-out tree. `doc` provides ComputedStyle.
class DisplayListBuilder {
public:
    DisplayList build(malibu::dom::Document& doc, malibu::layout::LayoutBox* root, float scroll_y = 0.0f);
};

} // namespace malibu::render
