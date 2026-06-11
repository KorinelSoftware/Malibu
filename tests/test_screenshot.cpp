// tests/test_screenshot.cpp
// End-to-end: render a real HTML+CSS+JS page to a PNG and verify the pixels.

#include <gtest/gtest.h>
#include "malibu/view/view.h"
#include "malibu/image/png.h"

#include <cstdio>
#include <string>

using malibu::view::View;

TEST(Screenshot, RendersPageToPngAndReadsBack) {
    View v;
    ASSERT_TRUE(v.load_html(
        "<body style='margin:0; background-color:#ffffff'>"
        "<div style='background-color:#2266ff; width:80px; height:80px'></div>"
        "</body>",
        "https://example.com/"));
    auto fb = v.render(120, 120);

    const std::string path = "test_screenshot_out.png";
    ASSERT_TRUE(malibu::image::write_png_rgba(path, fb.rgba.data(), fb.width, fb.height));

    // Read it back and verify dimensions + colours.
    auto img = malibu::image::read_png(path);
    ASSERT_TRUE(img.ok);
    EXPECT_EQ(img.width, 120);
    EXPECT_EQ(img.height, 120);
    auto channel = [&](int x, int y, int c) {
        return static_cast<int>(img.rgba[(static_cast<size_t>(y) * img.width + x) * 4 + c]);
    };
    // Blue box #2266ff.
    EXPECT_EQ(channel(40, 40, 0), 0x22);
    EXPECT_EQ(channel(40, 40, 1), 0x66);
    EXPECT_EQ(channel(40, 40, 2), 0xff);
    // White background.
    EXPECT_EQ(channel(110, 110, 0), 255);
    EXPECT_EQ(channel(110, 110, 1), 255);
    EXPECT_EQ(channel(110, 110, 2), 255);

    std::remove(path.c_str());
}

TEST(Screenshot, RendersTextPage) {
    View v;
    v.load_html("<body style='margin:0'><p style='font-size:48px; color:#000000'>Hi</p></body>",
                "https://example.com/");
    auto fb = v.render(200, 80);
    const std::string path = "test_screenshot_text.png";
    ASSERT_TRUE(malibu::image::write_png_rgba(path, fb.rgba.data(), fb.width, fb.height));
    auto img = malibu::image::read_png(path);
    ASSERT_TRUE(img.ok);
    // Real glyphs were drawn → some dark pixels exist.
    int dark = 0;
    for (size_t i = 0; i + 2 < img.rgba.size(); i += 4)
        if (img.rgba[i] < 100 && img.rgba[i + 1] < 100 && img.rgba[i + 2] < 100) ++dark;
    EXPECT_GT(dark, 20);
    std::remove(path.c_str());
}

TEST(Screenshot, InlineSvgRetainsHtmlNormalizedViewBox) {
    View view;
    ASSERT_TRUE(view.load_html(
        "<style>"
        "body{margin:0}"
        "svg{display:block;width:128px;height:128px}"
        "</style>"
        "<svg viewBox='0 0 216 216'>"
        "<path fill='#d93900' "
        "d='M108 0 C48 0 0 48 0 108 C0 168 48 216 108 216 "
        "C168 216 216 168 216 108 C216 48 168 0 108 0 Z'/>"
        "</svg>",
        "https://example.com/"));

    auto fb = view.render(160, 160);
    int orange_pixels = 0;
    for (int y = 0; y < fb.height; ++y) {
        for (int x = 0; x < fb.width; ++x) {
            const auto pixel = fb.at(x, y);
            if (pixel.r > 150 && pixel.g < 100 && pixel.b < 80)
                ++orange_pixels;
        }
    }
    EXPECT_GT(orange_pixels, 4000);
}

TEST(Screenshot, SvgDecoderAcceptsLowercaseViewboxFromHtmlDom) {
    const std::string svg =
        "<svg viewbox='0 0 216 216'>"
        "<circle fill='#d93900' cx='108' cy='108' r='100'></circle>"
        "</svg>";
    auto image = malibu::image::decode_svg(
        reinterpret_cast<const uint8_t*>(svg.data()), svg.size(), 64, 64);
    ASSERT_TRUE(image.ok);
    int opaque_pixels = 0;
    for (size_t i = 3; i < image.rgba.size(); i += 4) {
        if (image.rgba[i] != 0) ++opaque_pixels;
    }
    EXPECT_GT(opaque_pixels, 2000);
}

TEST(Screenshot, GeneratedBeforeBoxUsesFlexStaticPosition) {
    View view;
    ASSERT_TRUE(view.load_html(
        "<style>"
        "body{margin:0}"
        "#host{position:relative;display:flex;align-items:center;"
        "justify-content:center;width:100px;height:100px}"
        "#host::before{content:'';position:absolute;width:40px;height:40px;"
        "border-radius:50%;background:#d93900}"
        "</style><div id='host'></div>",
        "https://example.com/"));
    auto fb = view.render(120, 120);
    EXPECT_EQ(fb.at(50, 50), (malibu::render::Color{217, 57, 0, 255}));
    EXPECT_EQ(fb.at(10, 10), (malibu::render::Color{255, 255, 255, 255}));
}
