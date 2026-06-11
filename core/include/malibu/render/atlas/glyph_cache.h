#pragma once
// core/include/malibu/render/atlas/glyph_cache.h
// CPU side of the glyph atlas: shelf-packing allocation + LRU eviction (Task 26
// / Req 8.2). The Vulkan backend uploads the rasterized glyph bitmaps into the
// GPU texture at the rects this cache hands out; eviction never drops a frame
// (the evicted glyph is simply re-rasterized on next use).

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace malibu::render {

struct GlyphKey {
    uint32_t font_id = 0;
    uint32_t codepoint = 0;
    bool operator==(const GlyphKey&) const noexcept = default;
};

struct GlyphKeyHash {
    size_t operator()(const GlyphKey& k) const noexcept {
        uint64_t v = (static_cast<uint64_t>(k.font_id) << 32) | k.codepoint;
        v ^= v >> 30; v *= 0xbf58476d1ce4e5b9ULL; v ^= v >> 27;
        v *= 0x94d049bb133111ebULL; v ^= v >> 31;
        return static_cast<size_t>(v);
    }
};

struct AtlasRect { uint32_t x = 0, y = 0, w = 0, h = 0; };

// Shelf packer: places rows ("shelves") of glyphs top to bottom.
class ShelfPacker {
public:
    ShelfPacker(uint32_t width, uint32_t height) : width_(width), height_(height) {}
    void reset() { shelf_x_ = shelf_y_ = shelf_h_ = 0; }
    bool allocate(uint32_t w, uint32_t h, AtlasRect& out);
private:
    uint32_t width_, height_;
    uint32_t shelf_x_ = 0, shelf_y_ = 0, shelf_h_ = 0;
};

class GlyphCache {
public:
    GlyphCache(uint32_t atlas_width, uint32_t atlas_height)
        : packer_(atlas_width, atlas_height), width_(atlas_width), height_(atlas_height) {}

    // Returns the atlas rect for (key); allocates a w×h slot if missing,
    // evicting least-recently-used glyphs (and repacking) if the atlas is full.
    // `frame` is a monotonically increasing frame counter for LRU.
    AtlasRect get_or_insert(GlyphKey key, uint32_t w, uint32_t h, uint64_t frame);

    [[nodiscard]] bool   contains(GlyphKey key) const { return slots_.count(key) != 0; }
    [[nodiscard]] size_t size() const noexcept { return slots_.size(); }
    [[nodiscard]] uint64_t eviction_count() const noexcept { return evictions_; }

private:
    struct Slot { AtlasRect rect; uint32_t w, h; uint64_t last_used; };
    bool repack_all();

    ShelfPacker                                          packer_;
    std::unordered_map<GlyphKey, Slot, GlyphKeyHash>     slots_;
    uint32_t width_, height_;
    uint64_t evictions_ = 0;
};

} // namespace malibu::render
