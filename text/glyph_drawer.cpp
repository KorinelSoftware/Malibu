// text/glyph_drawer.cpp
// Shapes a text run and blits anti-aliased glyph coverage into the framebuffer.

#include "malibu/text/glyph_drawer.h"

#include <algorithm>
#include <cmath>

namespace malibu::text {

using render::Framebuffer;
using render::PaintText;
using render::Transform2D;
using render::ClipRect;
using render::Color;

namespace {
void blend(Framebuffer& fb, int x, int y, Color c, float alpha, const ClipRect& clip) {
    if (alpha <= 0.0f) return;
    if (clip.active) {
        if (x < clip.x || y < clip.y || x >= clip.x + clip.w || y >= clip.y + clip.h) return;
    }
    if (x < 0 || y < 0 || x >= fb.width || y >= fb.height) return;
    if (alpha > 1.0f) alpha = 1.0f;
    size_t i = (static_cast<size_t>(y) * fb.width + x) * 4;
    float inv = 1.0f - alpha;
    fb.rgba[i + 0] = static_cast<uint8_t>(c.r * alpha + fb.rgba[i + 0] * inv);
    fb.rgba[i + 1] = static_cast<uint8_t>(c.g * alpha + fb.rgba[i + 1] * inv);
    fb.rgba[i + 2] = static_cast<uint8_t>(c.b * alpha + fb.rgba[i + 2] * inv);
    fb.rgba[i + 3] = static_cast<uint8_t>(std::min(255.0f, 255.0f * alpha + fb.rgba[i + 3] * inv));
}
}  // namespace

void FreeTypeTextDrawer::draw_text(Framebuffer& fb, const PaintText& text,
                                   const Transform2D& transform, float opacity,
                                   const ClipRect& clip) {
    // Per-glyph font fallback: shape each maximal run that maps to one font (the
    // primary family if it covers the char, else a fontconfig match). A run is
    // only reshaped with a fallback when BOTH the primary lacks the char AND the
    // fallback *and the primary's HarfBuzz shaping of the run together* produce
    // glyphs — verified by `run_renders` so primary-covered scripts are never
    // disturbed and a bad fallback never replaces legible (or tofu) output.
    auto primary = fonts_->get_font(family_, false, text.font_size);
    if (!primary) return;
    const std::u16string& s = text.text;

    // Choose the font for a run: primary unless it produces only .notdef (glyph
    // id 0) for the run, in which case try a fontconfig fallback that does not.
    auto run_font = [&](const std::u16string& run) -> Font* {
        auto pg = primary->shape(run);
        bool primary_ok = false;
        for (auto& g : pg) if (g.glyph_id != 0) { primary_ok = true; break; }
        if (primary_ok) return primary.get();
        // primary can't render this run — find a fallback by its first codepoint.
        char32_t cp = run.empty() ? 0 : run[0];
        if (cp >= 0xD800 && cp <= 0xDBFF && run.size() > 1) cp = 0x10000 + ((cp - 0xD800) << 10) + (run[1] - 0xDC00);
        auto fb = fonts_->cover_font(cp, false, text.font_size);
        if (!fb) return primary.get();
        auto fg = fb->shape(run);
        for (auto& g : fg) if (g.glyph_id != 0) return fb.get();
        return primary.get();
    };

    // Segment the string into script-homogeneous runs (consecutive chars in the
    // same Unicode block bucket) so a single run never mixes Latin + CJK.
    auto bucket = [](char16_t c) -> int {
        if (c < 0x80) return 0;                       // ASCII/Latin
        if (c < 0x0590) return 1;                     // Latin-ext/Greek/Cyrillic
        if (c >= 0xAC00 && c <= 0xD7A3) return 4;     // Hangul
        if (c >= 0x3040 && c <= 0x30FF) return 5;     // Kana
        if (c >= 0x4E00 && c <= 0x9FFF) return 6;     // CJK ideographs
        return 3;                                     // other
    };
    float pen_x = text.x;
    float baseline_y = text.y + primary->ascent();
    size_t i = 0;
    while (i < s.size()) {
        int b = bucket(s[i]);
        size_t start = i++;
        while (i < s.size() && bucket(s[i]) == b) ++i;
        std::u16string run = s.substr(start, i - start);
        Font* rf = run_font(run);
        for (const ShapedGlyph& sg : rf->shape(run)) {
            GlyphBitmap bmp;
            if (rf->rasterize(sg.glyph_id, bmp) && bmp.width > 0 && bmp.height > 0) {
                float gx = pen_x + sg.x_offset + static_cast<float>(bmp.left);
                float gy = baseline_y - sg.y_offset - static_cast<float>(bmp.top);
                for (int row = 0; row < bmp.height; ++row) {
                    for (int col = 0; col < bmp.width; ++col) {
                        uint8_t cov = bmp.coverage[static_cast<size_t>(row) * bmp.width + col];
                        if (!cov) continue;
                        float dx = gx + col, dy = gy + row;
                        int px = static_cast<int>(std::lround(transform.a * dx + transform.c * dy + transform.e));
                        int py = static_cast<int>(std::lround(transform.b * dx + transform.d * dy + transform.f));
                        blend(fb, px, py, text.color, (cov / 255.0f) * opacity, clip);
                    }
                }
            }
            pen_x += sg.x_advance;
        }
    }
}

} // namespace malibu::text
