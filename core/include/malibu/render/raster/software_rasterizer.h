#pragma once
// core/include/malibu/render/raster/software_rasterizer.h
// Reference CPU rasterizer: composites a display list into an RGBA8 framebuffer
// with alpha blending, transforms (translate/scale exact, others bounding-box),
// opacity, and clipping. This is the deterministic, testable rendering path;
// the Vulkan backend consumes the same display list on real hardware.

#include <cstdint>
#include <vector>

#include "malibu/render/display_list/display_list.h"

namespace malibu::render {

struct Framebuffer {
    int                  width = 0;
    int                  height = 0;
    std::vector<uint8_t> rgba;   // width*height*4, row-major, premultiplied? no — straight alpha

    Framebuffer() = default;
    Framebuffer(int w, int h) : width(w), height(h), rgba(static_cast<size_t>(w) * h * 4, 0) {}

    void clear(Color c) {
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
            rgba[i] = c.r; rgba[i + 1] = c.g; rgba[i + 2] = c.b; rgba[i + 3] = c.a;
        }
    }
    [[nodiscard]] Color at(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return {0, 0, 0, 0};
        size_t i = (static_cast<size_t>(y) * width + x) * 4;
        return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
    }
};

// Optional real-text drawer. When supplied, the rasterizer delegates Text items
// to it (shaped + rasterized glyphs); otherwise it fills the run's bounds as a
// solid block. Implemented by the text/ subsystem (FreeType/HarfBuzz).
class TextDrawer {
public:
    virtual ~TextDrawer() = default;
    virtual void draw_text(Framebuffer& fb, const PaintText& text, const Transform2D& transform,
                           float opacity, const ClipRect& clip) = 0;
};

class SoftwareRasterizer {
public:
    // Composites the (already sorted) display list onto `fb`, back to front.
    // If `text_drawer` is non-null, Text items are drawn as real glyphs.
    void rasterize(const DisplayList& list, Framebuffer& fb, TextDrawer* text_drawer = nullptr);
};

} // namespace malibu::render
