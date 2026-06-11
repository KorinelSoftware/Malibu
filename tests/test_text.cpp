// tests/test_text.cpp
// Real text engine: fontconfig resolution, HarfBuzz shaping, FreeType raster.

#include <gtest/gtest.h>
#include "malibu/text/font.h"
#include "malibu/text/text_measurer.h"
#include "malibu/text/glyph_drawer.h"
#include "malibu/render/raster/software_rasterizer.h"

using namespace malibu::text;

TEST(Text, FontSystemAvailable) {
    FontSystem fs;
    EXPECT_TRUE(fs.available());
    auto font = fs.default_font(16.0f);
    ASSERT_NE(font, nullptr);
    EXPECT_FLOAT_EQ(font->pixel_size(), 16.0f);
}

TEST(Text, FontMetricsAreSane) {
    FontSystem fs;
    auto font = fs.default_font(32.0f);
    ASSERT_NE(font, nullptr);
    EXPECT_GT(font->ascent(), 0.0f);
    EXPECT_GT(font->descent(), 0.0f);
    EXPECT_GT(font->line_height(), font->ascent());  // line height spans asc+desc(+gap)
}

TEST(Text, ShapingProducesGlyphsWithAdvances) {
    FontSystem fs;
    auto font = fs.default_font(20.0f);
    ASSERT_NE(font, nullptr);
    auto glyphs = font->shape(u"Hello");
    EXPECT_EQ(glyphs.size(), 5u);
    for (const auto& g : glyphs) {
        EXPECT_NE(g.glyph_id, 0u);     // a real glyph (not .notdef)
        EXPECT_GT(g.x_advance, 0.0f);
    }
}

TEST(Text, GlyphRasterizationHasCoverage) {
    FontSystem fs;
    auto font = fs.default_font(48.0f);
    ASSERT_NE(font, nullptr);
    auto glyphs = font->shape(u"A");
    ASSERT_EQ(glyphs.size(), 1u);
    GlyphBitmap bmp;
    ASSERT_TRUE(font->rasterize(glyphs[0].glyph_id, bmp));
    EXPECT_GT(bmp.width, 0);
    EXPECT_GT(bmp.height, 0);
    // 'A' at 48px must have some inked pixels.
    int inked = 0;
    for (uint8_t c : bmp.coverage) if (c > 0) ++inked;
    EXPECT_GT(inked, 0);
}

TEST(Text, MeasurerGivesRealAdvances) {
    FontSystem fs;
    FreeTypeTextMeasurer m(fs);
    float wi = m.char_advance(u'i', 32.0f);
    float wm = m.char_advance(u'm', 32.0f);
    EXPECT_GT(wi, 0.0f);
    EXPECT_GT(wm, wi);  // 'm' is wider than 'i' in a proportional font
}

TEST(Text, GlyphDrawerProducesPixels) {
    FontSystem fs;
    FreeTypeTextDrawer drawer(fs);
    malibu::render::Framebuffer fb(200, 60);
    fb.clear({255, 255, 255, 255});

    malibu::render::PaintText text;
    text.x = 5; text.y = 5;
    text.text = u"Malibu";
    text.color = {0, 0, 0, 255};
    text.font_size = 40.0f;
    drawer.draw_text(fb, text, malibu::render::Transform2D{}, 1.0f, malibu::render::ClipRect{});

    // Some pixels in the text region must be darkened (real glyphs were drawn).
    int dark = 0;
    for (int y = 0; y < fb.height; ++y)
        for (int x = 0; x < fb.width; ++x) {
            auto c = fb.at(x, y);
            if (c.r < 200) ++dark;
        }
    EXPECT_GT(dark, 50);
}
