#pragma once
// core/include/malibu/text/glyph_drawer.h
// Draws shaped + rasterized glyphs into a framebuffer (render::TextDrawer).

#include <string>

#include "malibu/render/raster/software_rasterizer.h"
#include "malibu/text/font.h"

namespace malibu::text {

class FreeTypeTextDrawer : public render::TextDrawer {
public:
    explicit FreeTypeTextDrawer(FontSystem& fonts, std::string family = "sans-serif")
        : fonts_(&fonts), family_(std::move(family)) {}

    void draw_text(render::Framebuffer& fb, const render::PaintText& text,
                   const render::Transform2D& transform, float opacity,
                   const render::ClipRect& clip) override;

private:
    FontSystem*  fonts_;
    std::string  family_;
};

} // namespace malibu::text
