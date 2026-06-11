#pragma once
// core/include/malibu/layout/layout_engine.h
// Builds a layout tree from the DOM + ComputedStyle and computes positions and
// sizes for block, inline, and flex formatting contexts (Task 12 / Req 7.x).

#include <cstdint>
#include <deque>
#include <unordered_map>

#include "layout_box.h"

namespace malibu::dom { class Document; }

namespace malibu::layout {

// Text metrics provider. The default is a deterministic monospace approximation
// (real metrics come from the Platform font system on a live backend).
class TextMeasurer {
public:
    virtual ~TextMeasurer() = default;
    virtual float char_advance(char16_t c, float font_size) const {
        (void)c;
        return font_size * 0.5f;
    }
    virtual float line_height(float font_size, float lh_multiple) const {
        return font_size * lh_multiple;
    }
};

class LayoutEngine {
public:
    LayoutEngine();

    void set_text_measurer(const TextMeasurer* m) { measurer_ = m ? m : &default_measurer_; }

    // Builds the layout tree from the document (using each element's
    // ComputedStyle) and lays it out into a viewport of the given size.
    // Returns the root layout box (the initial containing block).
    LayoutBox* layout_document(malibu::dom::Document& doc, float viewport_width,
                               float viewport_height);

    [[nodiscard]] LayoutBox* box_for_node(malibu::NodeHandle h) const;
    [[nodiscard]] LayoutBox* root() const noexcept { return root_; }

    void clear_replaced_intrinsic_sizes() { replaced_intrinsic_sizes_.clear(); }
    void set_replaced_intrinsic_size(malibu::NodeHandle h, float width,
                                     float height);

    // Topmost element box whose border-box contains the document-space point
    // (x, y). Used for hit-testing mouse input. Null if nothing is hit.
    [[nodiscard]] LayoutBox* hit_test(float x, float y) const;

    // Invalidation hooks (Req 7.5, 7.6).
    void mark_dirty(malibu::NodeHandle h);
    void incremental_layout(malibu::dom::Document& doc);
    void force_layout_for_read(malibu::dom::Document& doc);

private:
    LayoutBox*  alloc_box();
    LayoutBox*  build_box(malibu::dom::Document& doc, malibu::NodeHandle node);

    // Formatting contexts. `cb_width` is the containing block's content width.
    void  layout_block(LayoutBox* box, float cb_width);
    float layout_children(LayoutBox* box);   // returns content height
    float layout_inline_run(const std::vector<LayoutBox*>& items, LayoutBox* container,
                            float origin_x, float origin_y, float avail_width);
    void  layout_flex(LayoutBox* box);
    void  layout_table(LayoutBox* box);            // CSS table grid layout
    void  layout_grid(LayoutBox* box);             // CSS grid (grid-template-columns)
    void  layout_box_contents(LayoutBox* box);     // dispatch by box type (block/flex/table)

    void resolve_edges(LayoutBox* box, float cb_width);
    [[nodiscard]] float font_size_of(const LayoutBox* box) const;

    // Active floats in the current block formatting context (document space).
    // Propagated into descendant blocks so their line boxes wrap around floats;
    // `bfc_base_` marks where the current BFC's floats start (descendants below a
    // new BFC root don't see ancestor floats).
    struct FloatRec { malibu::css::FloatType side; float top, bottom, inner; };
    std::vector<FloatRec>                       floats_;
    size_t                                      bfc_base_ = 0;

    std::deque<LayoutBox>                       pool_;
    std::unordered_map<uint64_t, LayoutBox*>    node_to_box_;
    struct IntrinsicSize { float width = 0, height = 0; };
    std::unordered_map<uint64_t, IntrinsicSize> replaced_intrinsic_sizes_;
    LayoutBox*                                  root_ = nullptr;
    const TextMeasurer*                         measurer_ = nullptr;
    TextMeasurer                                default_measurer_;
    float                                       viewport_w_ = 0;
    float                                       viewport_h_ = 0;
    bool                                        needs_layout_ = true;
    malibu::dom::Document*                       doc_ = nullptr;

    static uint64_t key(malibu::NodeHandle h) {
        return (static_cast<uint64_t>(h.index) << 32) | h.generation;
    }
};

} // namespace malibu::layout
