// render/renderer.cpp
// Ties the display-list builder to the software rasterizer.

#include "malibu/render/renderer.h"

namespace malibu::render {

DisplayList Renderer::build_display_list(malibu::dom::Document& doc, malibu::layout::LayoutBox* root,
                                         float scroll_y) {
    DisplayListBuilder builder;
    return builder.build(doc, root, scroll_y);
}

Framebuffer Renderer::render(malibu::dom::Document& doc, malibu::layout::LayoutBox* root,
                            int width, int height, Color background, TextDrawer* text_drawer,
                            float scroll_y) {
    Framebuffer fb(width, height);
    fb.clear(background);
    DisplayList list = build_display_list(doc, root, scroll_y);
    SoftwareRasterizer raster;
    raster.rasterize(list, fb, text_drawer);
    return fb;
}

} // namespace malibu::render
