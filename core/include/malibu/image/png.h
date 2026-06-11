#pragma once
// core/include/malibu/image/png.h
// PNG encode/decode (libpng) — used to write rendered page screenshots and to
// verify them in tests. Lives outside core/ (touches files + libpng).

#include <cstdint>
#include <string>
#include <vector>

namespace malibu::image {

// Writes an RGBA8 (width*height*4, row-major, top-left origin) buffer to a PNG.
bool write_png_rgba(const std::string& path, const uint8_t* rgba, int width, int height);

struct DecodedImage {
    int                  width = 0;
    int                  height = 0;
    std::vector<uint8_t> rgba;   // width*height*4
    bool                 ok = false;
};

// Decodes a PNG back to RGBA8 (for verification / tests).
DecodedImage read_png(const std::string& path);

// Decodes a PNG from an in-memory byte buffer (used for fetched <img>).
DecodedImage decode_png(const uint8_t* data, size_t len);

// Decodes a JPEG from memory to RGBA8.
DecodedImage decode_jpeg(const uint8_t* data, size_t len);

// Decodes a WebP from memory to RGBA8 (libwebp via dlopen; ok=false if absent).
DecodedImage decode_webp(const uint8_t* data, size_t len);

// Auto-detects PNG/JPEG by magic and decodes.
DecodedImage decode_image(const uint8_t* data, size_t len);

// Rasterizes an SVG document to RGBA8 at the given pixel size (vector → needs a
// target box). Returns ok=false if the bytes aren't SVG.
DecodedImage decode_svg(const uint8_t* data, size_t len, int out_w, int out_h);

} // namespace malibu::image
