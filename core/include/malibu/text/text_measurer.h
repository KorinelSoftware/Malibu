#pragma once
// core/include/malibu/text/text_measurer.h
// Real font metrics for the layout engine (FreeType-backed).

#include <string>

#include "malibu/layout/layout_engine.h"
#include "malibu/text/font.h"

namespace malibu::text {

class FreeTypeTextMeasurer : public layout::TextMeasurer {
public:
    explicit FreeTypeTextMeasurer(FontSystem& fonts, std::string family = "sans-serif")
        : fonts_(&fonts), family_(std::move(family)) {}

    float char_advance(char16_t c, float font_size) const override {
        auto f = fonts_->get_font(family_, false, font_size);
        if (!f) return font_size * 0.5f;
        if (c >= 0x80 && !f->has_glyph(c)) {  // fall back for non-Latin glyphs
            if (auto fb = fonts_->cover_font(c, false, font_size)) return fb->char_advance(c);
        }
        return f->char_advance(c);
    }
    float line_height(float font_size, float lh_multiple) const override {
        return font_size * lh_multiple;  // CSS line-height governs; metrics inform baseline
    }

private:
    FontSystem*  fonts_;   // non-owning; const methods only read the pointer
    std::string  family_;
};

} // namespace malibu::text
