#pragma once
// core/include/malibu/render/vulkan/glyph_atlas.h
// GPU glyph atlas with LRU eviction.

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

namespace malibu::render::vulkan {

struct GlyphKey {
    uint32_t font_id;
    uint32_t codepoint;
    bool operator==(const GlyphKey&) const noexcept = default;
};

struct UVRect {
    float u0, v0, u1, v1;
};

struct GlyphKeyHash {
    size_t operator()(const GlyphKey& k) const noexcept;
};

class GlyphAtlas {
public:
    UVRect get_or_rasterize(const GlyphKey& key, class FontSystem& fonts);
private:
    struct AtlasEntry {
        GlyphKey key;
        UVRect uv;
        uint64_t last_used_frame = 0;
    };
    
    VkImage texture_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    uint32_t width_ = 0, height_ = 0;
    std::vector<AtlasEntry> entries_;
    // AtlasPacker packer_;
};

} // namespace malibu::render::vulkan