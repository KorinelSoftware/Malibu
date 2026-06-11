#pragma once
// core/include/malibu/css/computed_style/computed_style.h
// ComputedStyle: the resolved style of a DOM element after cascade, inheritance,
// and var() substitution (Task 11 / Requirement 6.3-6.9). All CSS public types
// live in namespace malibu::css.

#include <cstdint>
#include <string>
#include <unordered_map>

namespace malibu::css {

// ---------------------------------------------------------------------------
// Value types
// ---------------------------------------------------------------------------
enum class LengthUnit : uint8_t { Px, Em, Rem, Percent, Vw, Vh, Auto, Calc };

struct Length {
    float     value = 0.0f;
    LengthUnit unit = LengthUnit::Px;
    // calc() is linear: store per-unit coefficients (e.g. calc(100% - 40px) →
    // pct=100, px=-40). Resolved against the basis/font/viewport in resolve().
    float c_px = 0, c_pct = 0, c_em = 0, c_rem = 0, c_vw = 0, c_vh = 0;

    static Length px(float v)      { return {v, LengthUnit::Px, 0,0,0,0,0,0}; }
    static Length percent(float v) { return {v, LengthUnit::Percent, 0,0,0,0,0,0}; }
    static Length auto_()          { return {0.0f, LengthUnit::Auto, 0,0,0,0,0,0}; }

    [[nodiscard]] bool is_auto() const noexcept { return unit == LengthUnit::Auto; }
    [[nodiscard]] bool is_percent() const noexcept { return unit == LengthUnit::Percent; }

    // Resolves to CSS pixels given the font size (for em/rem) and the
    // percentage basis (containing-block size).
    [[nodiscard]] float resolve(float font_size, float percent_basis,
                                float root_font_size = 16.0f,
                                float viewport_w = 0.0f, float viewport_h = 0.0f) const {
        switch (unit) {
            case LengthUnit::Px:      return value;
            case LengthUnit::Em:      return value * font_size;
            case LengthUnit::Rem:     return value * root_font_size;
            case LengthUnit::Percent: return value / 100.0f * percent_basis;
            case LengthUnit::Vw:      return value / 100.0f * viewport_w;
            case LengthUnit::Vh:      return value / 100.0f * viewport_h;
            case LengthUnit::Auto:    return 0.0f;
            case LengthUnit::Calc:
                return c_px + c_em * font_size + c_rem * root_font_size +
                       c_pct / 100.0f * percent_basis +
                       c_vw / 100.0f * viewport_w + c_vh / 100.0f * viewport_h;
        }
        return 0.0f;
    }

    bool operator==(const Length&) const noexcept = default;
};

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    bool operator==(const Color&) const noexcept = default;
    [[nodiscard]] bool transparent() const noexcept { return a == 0; }
};

struct BoxEdge {
    Length top, right, bottom, left;
    bool operator==(const BoxEdge&) const noexcept = default;
};

// 2D affine transform (column vectors): [a c e; b d f].
struct Transform2D {
    float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    [[nodiscard]] bool is_identity() const noexcept {
        return a == 1 && b == 0 && c == 0 && d == 1 && e == 0 && f == 0;
    }
    bool operator==(const Transform2D&) const noexcept = default;
};

enum class DisplayType  : uint8_t { Block, Inline, InlineBlock, Flex, InlineFlex, ListItem,
                                    Table, TableRow, TableCell, TableRowGroup, Grid, InlineGrid, None };
enum class PositionType : uint8_t { Static, Relative, Absolute, Fixed, Sticky };
enum class VisibilityType : uint8_t { Visible, Hidden, Collapse };
enum class OverflowType  : uint8_t { Visible, Hidden, Scroll, Auto };
enum class ObjectFit     : uint8_t { Fill, Contain, Cover, None, ScaleDown };
enum class BoxSizing     : uint8_t { ContentBox, BorderBox };
enum class FlexDirection : uint8_t { Row, RowReverse, Column, ColumnReverse };
enum class FlexWrap      : uint8_t { NoWrap, Wrap, WrapReverse };
enum class AlignItems    : uint8_t { Stretch, FlexStart, FlexEnd, Center, Baseline };
enum class JustifyContent: uint8_t { FlexStart, FlexEnd, Center, SpaceBetween, SpaceAround, SpaceEvenly };
enum class TextAlign     : uint8_t { Left, Right, Center, Justify };
enum class FontWeight    : uint16_t { Normal = 400, Bold = 700 };
enum class FloatType     : uint8_t { None, Left, Right };
enum class ClearType     : uint8_t { None, Left, Right, Both };
enum class TextDecoration: uint8_t { None, Underline, LineThrough, Overline };
enum class FontStyle     : uint8_t { Normal, Italic, Oblique };
enum class ListStyleType : uint8_t { Disc, Circle, Square, Decimal, None };
enum class WhiteSpace    : uint8_t { Normal, NoWrap, Pre, PreWrap };
enum class TextTransform : uint8_t { None, Uppercase, Lowercase, Capitalize };
enum class VerticalAlign : uint8_t { Baseline, Top, Middle, Bottom, Sub, Super };

struct FlexProps {
    FlexDirection  direction = FlexDirection::Row;
    FlexWrap       wrap = FlexWrap::NoWrap;
    AlignItems     align_items = AlignItems::Stretch;
    JustifyContent justify_content = JustifyContent::FlexStart;
    float          grow = 0.0f;
    float          shrink = 1.0f;
    Length         basis = Length::auto_();
    bool operator==(const FlexProps&) const noexcept = default;
};

// ---------------------------------------------------------------------------
// ComputedStyle
// ---------------------------------------------------------------------------
struct ComputedStyle {
    // Box / layout
    DisplayType   display     = DisplayType::Inline;   // initial CSS value
    PositionType  position    = PositionType::Static;
    BoxSizing     box_sizing  = BoxSizing::ContentBox;
    Length        width       = Length::auto_();
    Length        height      = Length::auto_();
    Length        max_width   = Length::auto_();        // auto => none
    Length        min_width   = Length::auto_();        // auto => 0
    Length        max_height  = Length::auto_();
    Length        min_height  = Length::auto_();
    Length        top = Length::auto_(), right = Length::auto_(),
                  bottom = Length::auto_(), left = Length::auto_();  // insets (auto by default)
    BoxEdge       margin;
    BoxEdge       padding;
    BoxEdge       border;                              // border widths
    FlexProps     flex;
    OverflowType  overflow    = OverflowType::Visible;
    ObjectFit     object_fit  = ObjectFit::Fill;
    FloatType     float_      = FloatType::None;
    ClearType     clear       = ClearType::None;
    float         border_radius = 0.0f;
    float         aspect_ratio  = 0.0f;                 // width/height (0 = none) — replaced-element sizing
    std::u16string grid_template_columns;             // e.g. "1fr 2fr" / "repeat(3, 1fr)" / "200px auto"
    float          gap = 0.0f;                          // row/column gap (flex & grid)

    // Visual
    Color          color            = {0, 0, 0, 255};
    Color          svg_fill         = {0, 0, 0, 255};
    bool           svg_fill_current_color = false;
    bool           svg_fill_specified = false;
    Color          background_color  = {0, 0, 0, 0};
    Color          border_color      = {0, 0, 0, 255};
    Color          border_colors[4]  = {{0,0,0,255},{0,0,0,255},{0,0,0,255},{0,0,0,255}};  // per side T,R,B,L
    float          opacity           = 1.0f;
    Transform2D    transform;
    int32_t        z_index           = 0;
    bool           has_z_index       = false;          // auto vs explicit
    VisibilityType visibility        = VisibilityType::Visible;

    // background-image: linear-gradient(...). When `bg_gradient` is set the
    // renderer fills with the gradient (over background_color).
    struct GradientStop { Color color; float pos; };   // pos in [0,1]
    bool                       bg_gradient = false;
    float                      bg_angle = 180.0f;       // deg; 180 = to bottom
    std::vector<GradientStop>  bg_stops;

    // box-shadow (single, outset). offset + blur + color.
    bool   has_box_shadow = false;
    float  shadow_x = 0, shadow_y = 0, shadow_blur = 0, shadow_spread = 0;
    Color  shadow_color = {0, 0, 0, 0};

    // Text / font (inherited)
    float          font_size   = 16.0f;
    FontWeight     font_weight = FontWeight::Normal;
    std::u16string font_family = u"sans-serif";
    float          line_height = 1.2f;                 // multiple of font-size
    TextAlign      text_align  = TextAlign::Left;
    TextDecoration text_decoration = TextDecoration::None;
    FontStyle      font_style  = FontStyle::Normal;
    ListStyleType  list_style  = ListStyleType::Disc;
    WhiteSpace     white_space = WhiteSpace::Normal;
    TextTransform  text_transform = TextTransform::None;
    VerticalAlign  vertical_align = VerticalAlign::Baseline;

    // CSS custom properties declared on this element (resolved lazily).
    std::unordered_map<std::u16string, std::u16string> custom_props;

    // The document-default style (used as the cascade base / inheritance root).
    static ComputedStyle initial() { return ComputedStyle{}; }
};

} // namespace malibu::css
