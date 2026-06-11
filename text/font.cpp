// text/font.cpp
// FreeType + HarfBuzz + fontconfig implementation of the text engine.

#include "malibu/text/font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>
#include <fontconfig/fontconfig.h>

#include <map>

namespace malibu::text {

struct Font::Impl {
    FT_Face   face = nullptr;
    hb_font_t* hb = nullptr;
    float      size = 16.0f;
    ~Impl() {
        if (hb) hb_font_destroy(hb);
        if (face) FT_Done_Face(face);
    }
};

Font::Font(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Font::~Font() = default;

std::vector<ShapedGlyph> Font::shape(const std::u16string& text) {
    std::vector<ShapedGlyph> out;
    if (!impl_ || !impl_->hb || text.empty()) return out;

    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf16(buf, reinterpret_cast<const uint16_t*>(text.data()),
                        static_cast<int>(text.size()), 0, static_cast<int>(text.size()));
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_guess_segment_properties(buf);
    hb_shape(impl_->hb, buf, nullptr, 0);

    unsigned int n = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &n);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &n);
    out.reserve(n);
    for (unsigned int i = 0; i < n; ++i) {
        ShapedGlyph g;
        g.glyph_id  = info[i].codepoint;          // after shaping this is a glyph id
        g.x_advance = pos[i].x_advance / 64.0f;
        g.x_offset  = pos[i].x_offset / 64.0f;
        g.y_offset  = pos[i].y_offset / 64.0f;
        out.push_back(g);
    }
    hb_buffer_destroy(buf);
    return out;
}

bool Font::rasterize(uint32_t glyph_id, GlyphBitmap& out) {
    if (!impl_ || !impl_->face) return false;
    if (FT_Load_Glyph(impl_->face, glyph_id, FT_LOAD_RENDER) != 0) return false;
    FT_GlyphSlot g = impl_->face->glyph;
    out.width = static_cast<int>(g->bitmap.width);
    out.height = static_cast<int>(g->bitmap.rows);
    out.left = g->bitmap_left;
    out.top = g->bitmap_top;
    out.coverage.assign(static_cast<size_t>(out.width) * out.height, 0);
    for (int y = 0; y < out.height; ++y) {
        const unsigned char* row = g->bitmap.buffer + y * g->bitmap.pitch;
        for (int x = 0; x < out.width; ++x)
            out.coverage[static_cast<size_t>(y) * out.width + x] = row[x];
    }
    return true;
}

float Font::char_advance(char16_t c) {
    if (!impl_ || !impl_->face) return impl_ ? impl_->size * 0.5f : 8.0f;
    FT_UInt idx = FT_Get_Char_Index(impl_->face, c);
    if (FT_Load_Glyph(impl_->face, idx, FT_LOAD_DEFAULT) != 0) return impl_->size * 0.5f;
    return impl_->face->glyph->advance.x / 64.0f;
}

bool Font::has_glyph(char32_t c) const {
    return impl_ && impl_->face && FT_Get_Char_Index(impl_->face, c) != 0;
}

float Font::ascent() const  { return impl_ && impl_->face ? impl_->face->size->metrics.ascender / 64.0f : 0; }
float Font::descent() const { return impl_ && impl_->face ? -impl_->face->size->metrics.descender / 64.0f : 0; }
float Font::line_height() const { return impl_ && impl_->face ? impl_->face->size->metrics.height / 64.0f : impl_->size * 1.2f; }
float Font::pixel_size() const { return impl_ ? impl_->size : 0; }

// ---------------------------------------------------------------------------
// FontSystem
// ---------------------------------------------------------------------------
struct FontSystem::Impl {
    FT_Library ft = nullptr;
    FcConfig*  fc = nullptr;
    std::map<std::string, std::shared_ptr<Font>> cache;  // key: family|bold|size
    ~Impl() {
        cache.clear();             // free faces before the library
        if (ft) FT_Done_FreeType(ft);
        if (fc) FcConfigDestroy(fc);
    }
};

FontSystem::FontSystem() : impl_(std::make_unique<Impl>()) {
    if (FT_Init_FreeType(&impl_->ft) != 0) impl_->ft = nullptr;
    impl_->fc = FcInitLoadConfigAndFonts();
}

FontSystem::~FontSystem() = default;

bool FontSystem::available() const { return impl_ && impl_->ft && impl_->fc; }

std::shared_ptr<Font> FontSystem::get_font(const std::string& family, bool bold, float pixel_size) {
    if (!available()) return nullptr;
    int px = static_cast<int>(pixel_size + 0.5f);
    if (px < 1) px = 1;
    std::string key = family + "|" + (bold ? "b" : "n") + "|" + std::to_string(px);
    auto it = impl_->cache.find(key);
    if (it != impl_->cache.end()) return it->second;

    // Resolve the family to a concrete font file via fontconfig.
    FcPattern* pat = FcNameParse(reinterpret_cast<const FcChar8*>(family.c_str()));
    if (bold) FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_BOLD);
    FcConfigSubstitute(impl_->fc, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern* match = FcFontMatch(impl_->fc, pat, &res);
    std::shared_ptr<Font> font;
    if (match) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
            auto fi = std::make_unique<Font::Impl>();
            fi->size = pixel_size;
            if (FT_New_Face(impl_->ft, reinterpret_cast<const char*>(file), 0, &fi->face) == 0) {
                FT_Set_Pixel_Sizes(fi->face, 0, static_cast<FT_UInt>(px));
                FT_Select_Charmap(fi->face, FT_ENCODING_UNICODE);  // so FT_Get_Char_Index works for non-ASCII
                fi->hb = hb_ft_font_create_referenced(fi->face);
                font = std::make_shared<Font>(std::move(fi));
            }
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    if (font) impl_->cache[key] = font;
    return font;
}

std::shared_ptr<Font> FontSystem::cover_font(char32_t c, bool bold, float pixel_size) {
    if (!available()) return nullptr;
    int px = static_cast<int>(pixel_size + 0.5f); if (px < 1) px = 1;
    std::string key = "cover:" + std::to_string(static_cast<uint32_t>(c)) + "|" + (bold ? "b" : "n") + "|" + std::to_string(px);
    auto it = impl_->cache.find(key);
    if (it != impl_->cache.end()) return it->second;

    // Ask fontconfig for a font whose charset covers `c`.
    FcPattern* pat = FcPatternCreate();
    FcCharSet* cs = FcCharSetCreate();
    FcCharSetAddChar(cs, static_cast<FcChar32>(c));
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    if (bold) FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_BOLD);
    FcConfigSubstitute(impl_->fc, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern* match = FcFontMatch(impl_->fc, pat, &res);
    std::shared_ptr<Font> font;
    if (match) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
            auto fi = std::make_unique<Font::Impl>();
            fi->size = pixel_size;
            if (FT_New_Face(impl_->ft, reinterpret_cast<const char*>(file), 0, &fi->face) == 0) {
                FT_Set_Pixel_Sizes(fi->face, 0, static_cast<FT_UInt>(px));
                FT_Select_Charmap(fi->face, FT_ENCODING_UNICODE);  // so FT_Get_Char_Index works for non-ASCII
                fi->hb = hb_ft_font_create_referenced(fi->face);
                font = std::make_shared<Font>(std::move(fi));
            }
        }
        FcPatternDestroy(match);
    }
    FcCharSetDestroy(cs);
    FcPatternDestroy(pat);
    impl_->cache[key] = font;   // cache even null to avoid repeated lookups
    return font;
}

} // namespace malibu::text
