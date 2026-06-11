#pragma once
// core/include/malibu/render/vulkan/frame_renderer.h
// Frame renderer - display list to Vulkan.

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>
#include "glyph_atlas.h"
#include "../../layout/layout_box.h"
#include "../../css/computed_style/computed_style.h"

namespace malibu::render::vulkan {

struct DrawCall {
    uint32_t z_index;
    uint32_t document_order;
    // draw parameters
};

class FrameRenderer {
public:
    void begin_frame(uint32_t swapchain_image_index);
    void render_box(const malibu::layout::LayoutBox& box, const malibu::css::computed_style::ComputedStyle& style);
    void render_text(const malibu::layout::LayoutBox& box, const malibu::css::computed_style::ComputedStyle& style,
                     const class TextRun& run, GlyphAtlas& atlas);
    void render_canvas2d(const class Canvas2DCommandList& cmds);
    void end_frame(VulkanSwapchain& sc);
private:
    std::vector<DrawCall> draw_calls_;
    VkCommandBuffer cmd_buf_ = VK_NULL_HANDLE;
};

} // namespace malibu::render::vulkan