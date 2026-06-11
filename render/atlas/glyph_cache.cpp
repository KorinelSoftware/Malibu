// render/atlas/glyph_cache.cpp
// Shelf packing + LRU eviction for the glyph atlas.

#include "malibu/render/atlas/glyph_cache.h"

#include <algorithm>
#include <vector>

namespace malibu::render {

bool ShelfPacker::allocate(uint32_t w, uint32_t h, AtlasRect& out) {
    if (w > width_ || h > height_) return false;
    if (shelf_x_ + w > width_) {  // start a new shelf
        shelf_y_ += shelf_h_;
        shelf_x_ = 0;
        shelf_h_ = 0;
    }
    if (shelf_y_ + h > height_) return false;
    out = AtlasRect{shelf_x_, shelf_y_, w, h};
    shelf_x_ += w;
    shelf_h_ = std::max(shelf_h_, h);
    return true;
}

bool GlyphCache::repack_all() {
    // Repack all current slots (in last-used order) into a fresh packer.
    std::vector<std::pair<GlyphKey, Slot*>> entries;
    entries.reserve(slots_.size());
    for (auto& [k, s] : slots_) entries.emplace_back(k, &s);
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.second->last_used < b.second->last_used; });
    packer_.reset();
    for (auto& [k, s] : entries) {
        AtlasRect r;
        if (!packer_.allocate(s->w, s->h, r)) return false;
        s->rect = r;
    }
    return true;
}

AtlasRect GlyphCache::get_or_insert(GlyphKey key, uint32_t w, uint32_t h, uint64_t frame) {
    auto it = slots_.find(key);
    if (it != slots_.end()) {
        it->second.last_used = frame;
        return it->second.rect;
    }

    AtlasRect rect;
    while (!packer_.allocate(w, h, rect)) {
        if (slots_.empty()) return AtlasRect{0, 0, 0, 0};  // glyph larger than atlas
        // Evict the least-recently-used glyph, then repack and retry.
        auto lru = std::min_element(slots_.begin(), slots_.end(),
                                    [](const auto& a, const auto& b) {
                                        return a.second.last_used < b.second.last_used;
                                    });
        slots_.erase(lru);
        ++evictions_;
        repack_all();
    }
    slots_[key] = Slot{rect, w, h, frame};
    return rect;
}

} // namespace malibu::render
