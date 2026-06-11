#pragma once
// core/include/malibu/text/font.h
// Real text shaping + rasterization: fontconfig (font resolution) + FreeType
// (glyph rasterization) + HarfBuzz (shaping). FreeType/HarfBuzz/fontconfig
// types are kept out of this header via a pimpl.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace malibu::text {

// A shaped glyph: a glyph id plus its placement (CSS pixels).
struct ShapedGlyph {
    uint32_t glyph_id = 0;
    float    x_advance = 0;
    float    x_offset = 0;
    float    y_offset = 0;
};

// An 8-bit coverage (alpha) bitmap for one glyph, plus its bearing.
struct GlyphBitmap {
    int                  width = 0;
    int                  height = 0;
    int                  left = 0;   // bearing-x from the pen
    int                  top = 0;    // bearing-y (above baseline)
    std::vector<uint8_t> coverage;   // width*height, 0..255
};

class Font {
public:
    struct Impl;
    explicit Font(std::unique_ptr<Impl> impl);
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    // Shapes a UTF-16 string into positioned glyphs (HarfBuzz).
    std::vector<ShapedGlyph> shape(const std::u16string& text);

    // Rasterizes a glyph id into `out` (FreeType). Returns false on failure.
    bool rasterize(uint32_t glyph_id, GlyphBitmap& out);

    // Horizontal advance (px) of a single character at this font's size.
    [[nodiscard]] float char_advance(char16_t c);

    // True if this face has a glyph for `c` (for fallback decisions).
    [[nodiscard]] bool has_glyph(char32_t c) const;

    [[nodiscard]] float ascent() const;
    [[nodiscard]] float descent() const;
    [[nodiscard]] float line_height() const;
    [[nodiscard]] float pixel_size() const;

private:
    std::unique_ptr<Impl> impl_;
};

class FontSystem {
public:
    FontSystem();
    ~FontSystem();
    FontSystem(const FontSystem&) = delete;
    FontSystem& operator=(const FontSystem&) = delete;

    [[nodiscard]] bool available() const;

    // Resolves a CSS font-family to a concrete face (fontconfig) and loads it
    // at the given pixel size. Cached. Returned fonts must not outlive `this`.
    std::shared_ptr<Font> get_font(const std::string& family, bool bold, float pixel_size);
    std::shared_ptr<Font> default_font(float pixel_size) { return get_font("sans-serif", false, pixel_size); }

    // Returns a font that has a glyph for codepoint `c` (fontconfig charset
    // match), for per-glyph fallback when the primary family lacks it (e.g.
    // CJK/Arabic/emoji text). Cached. Null if none found.
    std::shared_ptr<Font> cover_font(char32_t c, bool bold, float pixel_size);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace malibu::text
