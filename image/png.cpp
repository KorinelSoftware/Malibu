// image/png.cpp
// PNG encode/decode via libpng.

#include "malibu/image/png.h"

#include <png.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace malibu::image {

bool write_png_rgba(const std::string& path, const uint8_t* rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0) return false;
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) { std::fclose(fp); return false; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, nullptr); std::fclose(fp); return false; }

    if (setjmp(png_jmpbuf(png))) {  // libpng error handler
        png_destroy_write_struct(&png, &info);
        std::fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, static_cast<png_uint_32>(width), static_cast<png_uint_32>(height), 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<png_bytep> rows(static_cast<size_t>(height));
    for (int y = 0; y < height; ++y)
        rows[static_cast<size_t>(y)] = const_cast<png_bytep>(rgba + static_cast<size_t>(y) * width * 4);
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);

    png_destroy_write_struct(&png, &info);
    std::fclose(fp);
    return true;
}

namespace {
// Decode body shared by the file and memory readers. `png`/`info` are already
// created and the input source (png_init_io / png_set_read_fn) already set.
DecodedImage decode_common(png_structp png, png_infop info) {
    DecodedImage out;
    png_read_info(png, info);
    png_uint_32 w = 0, h = 0;
    int bit_depth = 0, color_type = 0;
    png_get_IHDR(png, info, &w, &h, &bit_depth, &color_type, nullptr, nullptr, nullptr);

    // Normalize to 8-bit RGBA.
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_read_update_info(png, info);

    out.width = static_cast<int>(w);
    out.height = static_cast<int>(h);
    out.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    std::vector<png_bytep> rows(h);
    for (png_uint_32 y = 0; y < h; ++y)
        rows[y] = out.rgba.data() + static_cast<size_t>(y) * w * 4;
    png_read_image(png, rows.data());
    png_read_end(png, nullptr);
    out.ok = true;
    return out;
}

struct MemSource { const uint8_t* p; size_t left; };
void mem_read(png_structp png, png_bytep out, png_size_t n) {
    MemSource* s = static_cast<MemSource*>(png_get_io_ptr(png));
    size_t k = n < s->left ? n : s->left;
    std::memcpy(out, s->p, k); s->p += k; s->left -= k;
}
}  // namespace

DecodedImage read_png(const std::string& path) {
    DecodedImage out;
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return out;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) { std::fclose(fp); return out; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, nullptr, nullptr); std::fclose(fp); return out; }
    if (setjmp(png_jmpbuf(png))) { png_destroy_read_struct(&png, &info, nullptr); std::fclose(fp); return DecodedImage{}; }
    png_init_io(png, fp);
    out = decode_common(png, info);
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(fp);
    return out;
}

DecodedImage decode_png(const uint8_t* data, size_t len) {
    DecodedImage out;
    if (!data || len < 8 || png_sig_cmp(data, 0, 8) != 0) return out;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) return out;
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, nullptr, nullptr); return out; }
    if (setjmp(png_jmpbuf(png))) { png_destroy_read_struct(&png, &info, nullptr); return DecodedImage{}; }
    MemSource src{data, len};
    png_set_read_fn(png, &src, mem_read);
    out = decode_common(png, info);
    png_destroy_read_struct(&png, &info, nullptr);
    return out;
}

} // namespace malibu::image
