#pragma once
// core/include/malibu/render/renderer.h
// CPU rendering facade: lays a document out into a display list and composites
// it into an RGBA8 framebuffer (the reference, fully-deterministic path). The
// Vulkan backend consumes the same display list on GPU.

#include "malibu/render/display_list/display_list.h"
#include "malibu/render/raster/software_rasterizer.h"

namespace malibu::dom { class Document; }
namespace malibu::layout { struct LayoutBox; }

namespace malibu::render {

class Renderer {
public:
    // Builds the display list for an already laid-out tree and rasterizes it.
    // `text_drawer` (optional) draws real glyphs; null falls back to blocks.
    Framebuffer render(malibu::dom::Document& doc, malibu::layout::LayoutBox* root,
                       int width, int height, Color background = {255, 255, 255, 255},
                       TextDrawer* text_drawer = nullptr, float scroll_y = 0.0f);

    // Just the display list (for the Vulkan backend or inspection).
    DisplayList build_display_list(malibu::dom::Document& doc, malibu::layout::LayoutBox* root,
                                   float scroll_y = 0.0f);
};

} // namespace malibu::render
