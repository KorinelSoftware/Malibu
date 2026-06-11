// layout/layout_engine.cpp
// Block, inline (with text wrapping), and flex formatting contexts.

#include <cstdlib>
#include <cstdio>
#include "malibu/layout/layout_engine.h"
#include "malibu/dom/document.h"
#include "malibu/diagnostics/diagnostic_log.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

namespace malibu::layout {

using malibu::css::ComputedStyle;
using malibu::css::DisplayType;
using malibu::css::BoxSizing;
using malibu::css::FlexDirection;
using malibu::css::AlignItems;
using malibu::css::JustifyContent;
using malibu::dom::Document;
using malibu::dom::NodeCore;

namespace {
float natural_text_width(LayoutBox* b, const TextMeasurer* m);  // defined below
// Margin-box intrinsic widths {min-content, max-content} of a subtree (CSS
// intrinsic sizing — the basis for flex-basis:auto, table columns, shrink-to-fit).
std::pair<float, float> intrinsic_widths(LayoutBox* b, const TextMeasurer* m);  // defined below
bool is_inline_level(BoxType t) {
    return t == BoxType::Inline || t == BoxType::Text || t == BoxType::InlineBlock;
}
std::vector<std::u16string> split_words(const std::u16string& s) {
    std::vector<std::u16string> words;
    std::u16string cur;
    for (char16_t c : s) {
        if (c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == u'\f') {
            if (!cur.empty()) { words.push_back(cur); cur.clear(); }
        } else cur.push_back(c);
    }
    if (!cur.empty()) words.push_back(cur);
    return words;
}
}  // namespace

LayoutEngine::LayoutEngine() : measurer_(&default_measurer_) {}

LayoutBox* LayoutEngine::alloc_box() {
    pool_.emplace_back();
    return &pool_.back();
}

float LayoutEngine::font_size_of(const LayoutBox* box) const {
    return box && box->style ? box->style->font_size : 16.0f;
}

LayoutBox* LayoutEngine::box_for_node(malibu::NodeHandle h) const {
    auto it = node_to_box_.find(key(h));
    return it != node_to_box_.end() ? it->second : nullptr;
}

void LayoutEngine::set_replaced_intrinsic_size(malibu::NodeHandle h,
                                               float width, float height) {
    if (width <= 0 || height <= 0) return;
    replaced_intrinsic_sizes_[key(h)] = {width, height};
    needs_layout_ = true;
}

namespace {
// Deepest element box (has a style) whose border-box contains (x,y). Children
// are tested last-first so the topmost painted box wins.
LayoutBox* hit_recurse(LayoutBox* box, float x, float y) {
    for (auto it = box->children.rbegin(); it != box->children.rend(); ++it)
        if (LayoutBox* h = hit_recurse(*it, x, y)) return h;
    const float l = box->x - box->padding[3] - box->border[3];
    const float t = box->y - box->padding[0] - box->border[0];
    const float r = box->x + box->width + box->padding[1] + box->border[1];
    const float b = box->y + box->height + box->padding[2] + box->border[2];
    if (box->style && x >= l && x < r && y >= t && y < b) return box;
    return nullptr;
}
}  // namespace

LayoutBox* LayoutEngine::hit_test(float x, float y) const {
    return root_ ? hit_recurse(root_, x, y) : nullptr;
}

LayoutBox* LayoutEngine::build_box(Document& doc, malibu::NodeHandle node) {
    const NodeCore* c = doc.core(node);
    if (!c) return nullptr;

    if (c->node_type == malibu::dom::kTextNode) {
        // Skip pure-whitespace text between block elements (collapsed).
        bool only_ws = true;
        for (char16_t ch : c->text_content)
            if (ch != u' ' && ch != u'\t' && ch != u'\n' && ch != u'\r' && ch != u'\f') { only_ws = false; break; }
        if (only_ws) return nullptr;
        LayoutBox* box = alloc_box();
        box->node = node;
        box->type = BoxType::Text;
        box->text = c->text_content;
        return box;
    }
    if (c->node_type != malibu::dom::kElementNode) return nullptr;

    const ComputedStyle* style = c->computed_style;
    DisplayType d = style ? style->display : DisplayType::Block;
    if (d == DisplayType::None) return nullptr;  // excluded from the layout tree

    LayoutBox* box = alloc_box();
    box->node = node;
    box->style = style;
    switch (d) {
        case DisplayType::Inline:      box->type = BoxType::Inline; break;
        case DisplayType::InlineBlock: box->type = BoxType::InlineBlock; break;
        case DisplayType::Flex:
        case DisplayType::InlineFlex:  box->type = BoxType::Flex; break;
        case DisplayType::Table:       box->type = BoxType::Table; break;
        case DisplayType::TableRow:    box->type = BoxType::TableRow; break;
        case DisplayType::TableCell:   box->type = BoxType::TableCell; break;
        case DisplayType::TableRowGroup: box->type = BoxType::TableRowGroup; break;
        case DisplayType::ListItem:    box->type = BoxType::ListItem; break;
        case DisplayType::Grid:
        case DisplayType::InlineGrid:  box->type = BoxType::Grid; break;
        default:                       box->type = BoxType::Block; break;
    }
    node_to_box_[key(node)] = box;

    // Replaced elements contribute one principal box. Their implementation
    // subtree is painted by the image/canvas/media backend, not laid out as
    // ordinary HTML descendants.
    const bool replaced = c->tag_name == u"svg" ||
                          c->tag_name == u"canvas" ||
                          c->tag_name == u"img" ||
                          c->tag_name == u"video";
    box->is_replaced = replaced;
    if (auto it = replaced_intrinsic_sizes_.find(key(node));
        it != replaced_intrinsic_sizes_.end()) {
        box->intrinsic_width = it->second.width;
        box->intrinsic_height = it->second.height;
    }
    if (!replaced) {
        for (malibu::NodeHandle child : c->children) {
            if (LayoutBox* cb = build_box(doc, child)) {
                cb->parent = box;
                box->children.push_back(cb);
            }
        }
    }

    // Blockify an inline box that contains block-level children (CSS: a block
    // inside an inline splits it; we approximate by making the inline a block).
    // Without this, e.g. an inline <span> wrapping <div>s collapses to width 0.
    if (box->type == BoxType::Inline || box->type == BoxType::InlineBlock) {
        for (LayoutBox* ch : box->children) {
            BoxType t = ch->type;
            if (t != BoxType::Inline && t != BoxType::Text && t != BoxType::InlineBlock) {
                box->type = BoxType::Block;
                break;
            }
        }
    }
    return box;
}

void LayoutEngine::resolve_edges(LayoutBox* box, float cb_width) {
    const ComputedStyle* s = box->style;
    if (!s) { for (int i = 0; i < 4; ++i) box->margin[i] = box->padding[i] = box->border[i] = 0; return; }
    float fs = s->font_size;
    auto r = [&](const malibu::css::Length& l) {
        return l.is_auto() ? 0.0f : l.resolve(fs, cb_width, 16.0f, viewport_w_, viewport_h_);
    };
    box->margin[0] = r(s->margin.top);  box->margin[1] = r(s->margin.right);
    box->margin[2] = r(s->margin.bottom); box->margin[3] = r(s->margin.left);
    box->padding[0] = r(s->padding.top); box->padding[1] = r(s->padding.right);
    box->padding[2] = r(s->padding.bottom); box->padding[3] = r(s->padding.left);
    box->border[0] = r(s->border.top); box->border[1] = r(s->border.right);
    box->border[2] = r(s->border.bottom); box->border[3] = r(s->border.left);
}

namespace {
float replaced_ratio(const LayoutBox* box) {
    if (!box) return 0;
    if (box->style && box->style->aspect_ratio > 0)
        return box->style->aspect_ratio;
    if (box->intrinsic_width > 0 && box->intrinsic_height > 0)
        return box->intrinsic_width / box->intrinsic_height;
    return 0;
}

float compute_content_width(LayoutBox* box, float avail, float vw, float vh) {
    const ComputedStyle* s = box->style;
    float h_extra = box->margin[1] + box->margin[3] + box->border[1] + box->border[3] + box->padding[1] + box->padding[3];
    float fs = s ? s->font_size : 16.0f;
    float w;
    bool explicit_w = s && !s->width.is_auto();
    if (explicit_w) {
        w = s->width.resolve(fs, avail, 16.0f, vw, vh);
        if (s->box_sizing == BoxSizing::BorderBox)
            w -= (box->padding[1] + box->padding[3] + box->border[1] + box->border[3]);
    } else if (box->is_replaced && box->intrinsic_width > 0) {
        w = box->intrinsic_width;
    } else {
        w = std::max(0.0f, avail - h_extra);
    }
    // Replaced-element sizing: width:auto + explicit height + aspect-ratio → derive width.
    const float ratio = replaced_ratio(box);
    if (s && s->width.is_auto() && ratio > 0 &&
        !s->height.is_auto() && !s->height.is_percent()) {
        float hh = s->height.resolve(fs, 0, 16.0f, vw, vh);
        w = hh * ratio;
    }
    // max-width / min-width (content-box). max-width can shrink an otherwise-full box.
    if (s && !s->max_width.is_auto()) w = std::min(w, std::max(0.0f, s->max_width.resolve(fs, avail, 16.0f, vw, vh)));
    if (s && !s->min_width.is_auto()) w = std::max(w, s->min_width.resolve(fs, avail, 16.0f, vw, vh));
    w = std::max(0.0f, w);
    // margin:auto horizontal centering (now that the final width is known).
    if (s && s->margin.left.is_auto() && s->margin.right.is_auto()) {
        float free = avail - (box->border[1] + box->border[3] + box->padding[1] + box->padding[3] + w);
        float m = std::max(0.0f, free / 2.0f);
        box->margin[1] = box->margin[3] = m;
    }
    return w;
}

float compute_height(LayoutBox* box, float content_h, float vw, float vh) {
    const ComputedStyle* s = box->style;
    float fs = s ? s->font_size : 16.0f;
    float h = content_h;
    if (s && !s->height.is_auto() && !s->height.is_percent()) {
        h = s->height.resolve(fs, 0.0f, 16.0f, vw, vh);
        if (s->box_sizing == BoxSizing::BorderBox)
            h -= (box->padding[0] + box->padding[2] + box->border[0] + box->border[2]);
    } else if (s && s->height.is_percent()) {
        // CSS §10.5: a percentage height resolves only against a containing block
        // with a DEFINITE height (an explicit px/etc. ancestor, or the viewport at
        // the root). Otherwise it behaves as auto (content height). This makes the
        // ubiquitous `height:100%` / `height:50%` patterns work.
        const LayoutBox* p = box->parent;
        float cbh = 0.0f;
        if (p && p->style && !p->style->height.is_auto() && !p->style->height.is_percent()) {
            cbh = p->style->height.resolve(p->style->font_size, 0.0f, 16.0f, vw, vh);
            if (p->style->box_sizing == BoxSizing::BorderBox)
                cbh -= (p->padding[0] + p->padding[2] + p->border[0] + p->border[2]);
        } else if (!p) {
            cbh = vh;  // initial containing block = viewport
        } else if (p->height > 0.0f && p->style && p->style->height.is_percent()) {
            cbh = p->height;  // parent's % already resolved against a definite chain
        }
        if (cbh > 0.0f) {
            h = s->height.resolve(fs, cbh, 16.0f, vw, vh);
            if (s->box_sizing == BoxSizing::BorderBox)
                h -= (box->padding[0] + box->padding[2] + box->border[0] + box->border[2]);
        }
    } else if (s && s->height.is_auto() && replaced_ratio(box) > 0 &&
               box->width > 0) {
        h = box->width / replaced_ratio(box);
    } else if (box->is_replaced && box->intrinsic_height > 0 &&
               (!s || s->height.is_auto())) {
        h = box->intrinsic_height;
    }
    if (s && !s->max_height.is_auto() && !s->max_height.is_percent()) h = std::min(h, std::max(0.0f, s->max_height.resolve(fs, 0.0f, 16.0f, vw, vh)));
    if (s && !s->min_height.is_auto() && !s->min_height.is_percent()) h = std::max(h, s->min_height.resolve(fs, 0.0f, 16.0f, vw, vh));
    return std::max(0.0f, h);
}
}  // namespace

float LayoutEngine::layout_inline_run(const std::vector<LayoutBox*>& items, LayoutBox* container,
                                      float origin_x, float origin_y, float avail_width) {
    float max_x = origin_x + avail_width;
    float cursor_x = origin_x;
    float line_top = origin_y;
    float line_max_h = 0.0f;
    float base_fs = container && container->style ? container->style->font_size : 16.0f;
    float base_lh = container && container->style ? container->style->line_height : 1.2f;

    auto newline = [&]() {
        line_top += (line_max_h > 0 ? line_max_h : measurer_->line_height(base_fs, base_lh));
        cursor_x = origin_x;
        line_max_h = 0.0f;
    };

    // Recursive lambda to flow inline atoms (text words / inline elements / inline-blocks).
    std::function<void(LayoutBox*, float, float)> flow = [&](LayoutBox* item, float fs, float lh) {
        if (item->type == BoxType::Text) {
            float space_w = measurer_->char_advance(u' ', fs);
            float lineh = measurer_->line_height(fs, lh);
            float start_y = line_top;
            item->x = cursor_x;
            item->y = line_top;
            item->fragments.clear();
            // Accumulate words into per-line fragments so wrapped text is drawn
            // line-by-line (instead of one overflowing run).
            std::u16string line; float line_x = cursor_x, line_y = line_top;
            auto flush = [&]() {
                if (!line.empty()) item->fragments.push_back({line, line_x, line_y, std::max(0.0f, cursor_x - space_w - line_x)});
                line.clear();
            };
            for (const std::u16string& word : split_words(item->text)) {
                float w = 0.0f;
                for (char16_t ch : word) w += measurer_->char_advance(ch, fs);
                if (cursor_x + w > max_x && cursor_x > origin_x) {   // wrap
                    flush();
                    newline();
                    line_x = cursor_x; line_y = line_top;
                }
                if (!line.empty()) line.push_back(u' ');
                line += word;
                cursor_x += w + space_w;
                line_max_h = std::max(line_max_h, lineh);
            }
            flush();
            item->width = std::max(0.0f, cursor_x - item->x);
            item->height = (line_top + (line_max_h > 0 ? line_max_h : lineh)) - start_y;
        } else if (item->type == BoxType::Inline) {
            float ifs = item->style ? item->style->font_size : fs;
            float ilh = item->style ? item->style->line_height : lh;
            item->x = cursor_x; item->y = line_top;
            for (LayoutBox* ch : item->children) flow(ch, ifs, ilh);
            item->width = std::max(0.0f, cursor_x - item->x);
            item->height = line_max_h > 0 ? line_max_h : measurer_->line_height(ifs, ilh);
        } else if (item->type == BoxType::InlineBlock) {
            resolve_edges(item, avail_width);
            // Inline-block is shrink-to-fit: auto width = max-content (clamped),
            // NOT fill — otherwise each inline-block takes a full line and they
            // stack vertically instead of flowing in a row.
            if (item->style && !item->style->width.is_auto()) {
                item->width = compute_content_width(item, avail_width, viewport_w_, viewport_h_);
            } else {
                float fe = item->margin[1] + item->margin[3] + item->border[1] + item->border[3] + item->padding[1] + item->padding[3];
                auto [imn, imx] = intrinsic_widths(item, measurer_); (void)imn;
                item->width = std::max(0.0f, std::min(avail_width - fe, imx - fe));
                if (item->style && !item->style->max_width.is_auto())
                    item->width = std::min(item->width, std::max(0.0f, item->style->max_width.resolve(item->style->font_size, avail_width, 16.0f, viewport_w_, viewport_h_)));
            }
            float chh = layout_children(item);    // pass 1: measure height
            item->height = compute_height(item, chh, viewport_w_, viewport_h_);
            float atom_w = item->margin_box_width();
            float atom_h = item->margin_box_height();
            if (cursor_x + atom_w > max_x && cursor_x > origin_x) newline();
            item->x = cursor_x + item->margin[3] + item->border[3] + item->padding[3];
            item->y = line_top + item->margin[0] + item->border[0] + item->padding[0];
            layout_children(item);                 // pass 2: re-place children at the final x/y
            cursor_x += atom_w;
            line_max_h = std::max(line_max_h, atom_h);
        }
    };

    for (LayoutBox* item : items) flow(item, base_fs, base_lh);
    return (line_top + line_max_h) - origin_y;
}

void LayoutEngine::layout_block(LayoutBox* box, float cb_width) {
    box->width = compute_content_width(box, cb_width, viewport_w_, viewport_h_);
    float content_h = layout_children(box);
    box->height = compute_height(box, content_h, viewport_w_, viewport_h_);
}

void LayoutEngine::layout_flex(LayoutBox* box) {
    const ComputedStyle* s = box->style;
    FlexDirection dir = s ? s->flex.direction : FlexDirection::Row;
    bool row = (dir == FlexDirection::Row || dir == FlexDirection::RowReverse);
    AlignItems align = s ? s->flex.align_items : AlignItems::Stretch;
    JustifyContent justify = s ? s->flex.justify_content : JustifyContent::FlexStart;

    std::vector<LayoutBox*> items;
    for (LayoutBox* c : box->children) if (c->type != BoxType::Text || !c->text.empty()) items.push_back(c);

    // Resolve each item's edges + base main size. For a row, an auto-width item's
    // flex-basis is its max-content (not the full container) — this is the proper
    // CSS model and stops auto items from over-shrinking. min_main[] is each
    // item's shrink floor (min-content; explicit sizes don't shrink below size).
    std::vector<float> min_main(items.size(), 0.0f);
    for (size_t k = 0; k < items.size(); ++k) {
        LayoutBox* it = items[k];
        resolve_edges(it, box->width);
        if (row) {
            float fe = it->margin[1] + it->margin[3] + it->border[1] + it->border[3] + it->padding[1] + it->padding[3];
            bool has_w = it->style && !it->style->width.is_auto();
            bool has_basis = it->style && !it->style->flex.basis.is_auto() && !it->style->flex.basis.is_percent();
            auto [imn, imx] = intrinsic_widths(it, measurer_);
            if (has_basis)      it->width = std::max(0.0f, it->style->flex.basis.resolve(it->style->font_size, box->width, 16.0f, viewport_w_, viewport_h_) - fe);
            else if (has_w)     it->width = compute_content_width(it, box->width, viewport_w_, viewport_h_);
            else                it->width = std::max(0.0f, imx - fe);          // flex-basis auto = max-content
            min_main[k] = (has_w || has_basis) ? it->width : std::max(0.0f, imn - fe);
        } else {
            it->width = compute_content_width(it, box->width, viewport_w_, viewport_h_);
        }
        if (it->type == BoxType::Flex) {
            it->height = compute_height(it, 0.0f, viewport_w_, viewport_h_);  // honor explicit height
            layout_flex(it);
        } else {
            float chh = layout_children(it);
            it->height = compute_height(it, chh, viewport_w_, viewport_h_);
        }
    }

    // flex-wrap (row): greedily pack items into multiple lines, stacking lines on
    // the cross axis. Items keep their measured sizes (grow/shrink/justify are not
    // applied per line — enough for card/nav grids, the common wrap case).
    if (s && s->flex.wrap != malibu::css::FlexWrap::NoWrap && row && !items.empty()) {
        float gap = s->gap;
        float x = box->x, y = box->y, line_h = 0;
        for (auto* it : items) {
            float w = it->margin_box_width();
            if (x > box->x && x + w > box->x + box->width + 0.5f) { x = box->x; y += line_h + gap; line_h = 0; }
            it->x = x + it->margin[3] + it->border[3] + it->padding[3];
            it->y = y + it->margin[0] + it->border[0] + it->padding[0];
            if (it->type == BoxType::Flex) layout_flex(it); else layout_children(it);
            x += w + gap;
            line_h = std::max(line_h, it->margin_box_height());
        }
        float content_h = (y + line_h) - box->y;
        if (!s || s->height.is_auto()) box->height = content_h;
        return;
    }

    // Main-axis available size. A definite size (explicit, or set by a parent
    // flex/stretch before we were called) wins; otherwise it's the content sum.
    float main_avail = row ? box->width : box->height;
    bool main_definite = row || box->height > 0 || (s && !s->height.is_auto() && !s->height.is_percent());
    if (!row && !main_definite) {
        float sum = 0; for (auto* it : items) sum += it->margin_box_height();
        main_avail = sum;  // column auto height = content
    }
    float used = 0;
    for (auto* it : items) used += row ? it->margin_box_width() : it->margin_box_height();
    // CSS `gap` occupies main-axis space between items (n-1 gaps), reducing the
    // free space available to grow/justify. (column-gap for row, row-gap for column.)
    const int item_count = static_cast<int>(items.size());
    float css_gap = (s && item_count > 1) ? s->gap : 0.0f;
    float free = main_avail - used - css_gap * (item_count - 1 > 0 ? item_count - 1 : 0);

    float grow_sum = 0, shrink_sum = 0;
    for (auto* it : items) {
        grow_sum += it->style ? it->style->flex.grow : 0.0f;
        shrink_sum += it->style ? it->style->flex.shrink : 1.0f;
    }
    if (free > 0 && grow_sum > 0) {
        for (auto* it : items) {
            float g = it->style ? it->style->flex.grow : 0.0f;
            float add = free * (g / grow_sum);
            if (row) it->width += add; else it->height += add;
        }
        used = main_avail; free = 0;
    } else if (free < 0 && shrink_sum > 0) {
        if (row) {
            // Weighted shrink (by flex-shrink × base) floored at each item's
            // min-content, redistributing over a few passes when items hit the floor.
            float need = -free;
            for (int pass = 0; pass < 5 && need > 0.5f; ++pass) {
                float wsum = 0;
                for (size_t k = 0; k < items.size(); ++k)
                    if (items[k]->width > min_main[k] + 0.5f)
                        wsum += (items[k]->style ? items[k]->style->flex.shrink : 1.0f) * items[k]->width;
                if (wsum <= 0) break;
                float dist = need; need = 0;
                for (size_t k = 0; k < items.size(); ++k) {
                    float cap = items[k]->width - min_main[k];
                    if (cap <= 0.5f) continue;
                    float sh = items[k]->style ? items[k]->style->flex.shrink : 1.0f;
                    float want = dist * (sh * items[k]->width / wsum);
                    float applied = std::min(want, cap);
                    items[k]->width -= applied;
                    need += want - applied;
                }
            }
        } else {
            for (auto* it : items) {
                float sh = it->style ? it->style->flex.shrink : 1.0f;
                it->height = std::max(0.0f, it->height - (-free) * (sh / shrink_sum));
            }
        }
        used = main_avail; free = 0;
    }

    // Cross-axis container size.
    float cross_size = row ? box->height : box->width;
    if (row && (!s || s->height.is_auto())) {
        float maxc = 0; for (auto* it : items) maxc = std::max(maxc, it->margin_box_height());
        cross_size = maxc;
        box->height = maxc;
    }

    // Justify-content: starting offset + inter-item gap. The CSS `gap` is the
    // baseline spacing; justify-content distributes the remaining free space
    // on top of it (space-* add to the gap; center/flex-end shift the start).
    float start = row ? box->x : box->y;
    float gap = css_gap;
    int n = item_count;
    if (n > 0) {
        switch (justify) {
            case JustifyContent::FlexEnd:      start += free; break;
            case JustifyContent::Center:       start += free / 2.0f; break;
            case JustifyContent::SpaceBetween: gap += n > 1 ? free / (n - 1) : 0; break;
            case JustifyContent::SpaceAround:  { float a = free / n; gap += a; start += a / 2.0f; } break;
            case JustifyContent::SpaceEvenly:  { float a = free / (n + 1); gap += a; start += a; } break;
            default: break;  // FlexStart
        }
    }

    float main_cursor = start;
    for (auto* it : items) {
        float mb_main = row ? it->margin_box_width() : it->margin_box_height();
        float mb_cross = row ? it->margin_box_height() : it->margin_box_width();
        // cross offset by align-items
        float cross_off = 0;
        switch (align) {
            case AlignItems::FlexEnd: cross_off = cross_size - mb_cross; break;
            case AlignItems::Center:  cross_off = (cross_size - mb_cross) / 2.0f; break;
            case AlignItems::Stretch:
                if (it->style && ((row && it->style->height.is_auto()) || (!row && it->style->width.is_auto()))) {
                    if (row) it->height = std::max(0.0f, cross_size - (it->margin[0] + it->margin[2] + it->border[0] + it->border[2] + it->padding[0] + it->padding[2]));
                    else     it->width  = std::max(0.0f, cross_size - (it->margin[1] + it->margin[3] + it->border[1] + it->border[3] + it->padding[1] + it->padding[3]));
                }
                break;
            default: break;  // FlexStart / Baseline → 0
        }
        if (row) {
            it->x = main_cursor + it->margin[3] + it->border[3] + it->padding[3];
            it->y = box->y + cross_off + it->margin[0] + it->border[0] + it->padding[0];
        } else {
            it->y = main_cursor + it->margin[0] + it->border[0] + it->padding[0];
            it->x = box->x + cross_off + it->margin[3] + it->border[3] + it->padding[3];
        }
        // Re-flow item children now that the item's final size is known.
        if (it->type == BoxType::Flex) layout_flex(it);
        else layout_children(it);
        main_cursor += mb_main + gap;
    }

    if (!row && !main_definite) box->height = used;  // only auto column boxes grow to content
}

// Lay out a box's own contents according to its formatting context.
void LayoutEngine::layout_box_contents(LayoutBox* box) {
    if (box->type == BoxType::Flex) { box->height = compute_height(box, 0.0f, viewport_w_, viewport_h_); layout_flex(box); }
    else if (box->type == BoxType::Table) { layout_table(box); }
    else if (box->type == BoxType::Grid) { layout_grid(box); }
    else { float chh = layout_children(box); box->height = compute_height(box, chh, viewport_w_, viewport_h_); }
}

float LayoutEngine::layout_children(LayoutBox* box) {
    float cursor_y = box->y;
    const float cb_left = box->x, cb_right = box->x + box->width;

    // Float formatting context (member-backed so floats propagate into descendant
    // blocks' line boxes). A new BFC root doesn't see ancestor floats.
    size_t saved_bfc = bfc_base_;
    size_t my_start = floats_.size();
    bool new_bfc = (box == root_) ||
                   (box->style && box->style->overflow != malibu::css::OverflowType::Visible) ||
                   (box->style && (box->style->float_ != malibu::css::FloatType::None ||
                                   box->style->position == malibu::css::PositionType::Absolute ||
                                   box->style->position == malibu::css::PositionType::Fixed)) ||
                   box->type == BoxType::InlineBlock || box->type == BoxType::TableCell;
    if (new_bfc) bfc_base_ = my_start;
    auto left_edge = [&](float y) {
        float e = cb_left;
        for (size_t k = bfc_base_; k < floats_.size(); ++k) { auto& f = floats_[k];
            if (f.side == malibu::css::FloatType::Left && y >= f.top - 0.5f && y < f.bottom) e = std::max(e, f.inner); }
        return e;
    };
    auto right_edge = [&](float y) {
        float e = cb_right;
        for (size_t k = bfc_base_; k < floats_.size(); ++k) { auto& f = floats_[k];
            if (f.side == malibu::css::FloatType::Right && y >= f.top - 0.5f && y < f.bottom) e = std::min(e, f.inner); }
        return e;
    };
    auto clear_to = [&](malibu::css::ClearType cl, float& y) {
        for (size_t k = bfc_base_; k < floats_.size(); ++k) { auto& f = floats_[k];
            bool m = (cl == malibu::css::ClearType::Both) ||
                     (cl == malibu::css::ClearType::Left && f.side == malibu::css::FloatType::Left) ||
                     (cl == malibu::css::ClearType::Right && f.side == malibu::css::FloatType::Right);
            if (m) y = std::max(y, f.bottom);
        }
    };

    float prev_mb = 0.0f;   // previous in-flow block's bottom margin (for collapsing)
    size_t i = 0;
    const auto& kids = box->children;
    while (i < kids.size()) {
        if (is_inline_level(kids[i]->type)) {
            std::vector<LayoutBox*> run;
            size_t j = i;
            while (j < kids.size() && is_inline_level(kids[j]->type)) run.push_back(kids[j++]);
            float l = left_edge(cursor_y), r = right_edge(cursor_y);
            float h = layout_inline_run(run, box, l, cursor_y, std::max(0.0f, r - l));
            cursor_y += h;
            prev_mb = 0.0f;
            i = j;
        } else {
            LayoutBox* c = kids[i];
            const ComputedStyle* cs = c->style;
            auto pos = cs ? cs->position : malibu::css::PositionType::Static;
            bool out_of_flow = (pos == malibu::css::PositionType::Absolute || pos == malibu::css::PositionType::Fixed);
            auto flt = cs ? cs->float_ : malibu::css::FloatType::None;
            resolve_edges(c, box->width);

            float fs = cs ? cs->font_size : 16.0f;
            auto off = [&](const malibu::css::Length& l) {
                return l.is_auto() ? 0.0f : l.resolve(fs, box->width, 16.0f, viewport_w_, viewport_h_);
            };

            if (out_of_flow) {
                // Containing block: for `absolute`, the nearest *positioned*
                // ancestor (else the initial containing block); for `fixed`, the
                // viewport. Offsets resolve against it; an auto inset keeps the box
                // at its static (in-flow) position rather than collapsing to 0,0.
                LayoutBox* cbx_box = root_;
                if (pos == malibu::css::PositionType::Absolute)
                    for (LayoutBox* p = box; p; p = p->parent)
                        if (p->style && p->style->position != malibu::css::PositionType::Static) { cbx_box = p; break; }
                float cbx = cbx_box ? cbx_box->x : box->x, cby = cbx_box ? cbx_box->y : box->y;
                float cbw = cbx_box ? cbx_box->width : box->width, cbh = cbx_box ? cbx_box->height : box->height;
                // The CB's computed height isn't known yet mid-layout; use its
                // explicit style height (or the viewport) so bottom/-anchoring works.
                if (cbx_box && cbx_box->style && !cbx_box->style->height.is_auto() && !cbx_box->style->height.is_percent())
                    cbh = cbx_box->style->height.resolve(cbx_box->style->font_size, 0, 16.0f, viewport_w_, viewport_h_);
                else if (cbx_box == root_) cbh = viewport_h_;
                if (pos == malibu::css::PositionType::Fixed) { cbx = 0; cby = 0; cbw = viewport_w_; cbh = viewport_h_; }
                // Insets resolve % against the containing block: width for
                // left/right, height for top/bottom.
                auto offw = [&](const malibu::css::Length& l) { return l.is_auto() ? 0.0f : l.resolve(fs, cbw, 16.0f, viewport_w_, viewport_h_); };
                auto offh = [&](const malibu::css::Length& l) { return l.is_auto() ? 0.0f : l.resolve(fs, cbh, 16.0f, viewport_w_, viewport_h_); };

                // width:auto on an out-of-flow box is shrink-to-fit (NOT fill), so
                // positioned boxes don't span their container and stack.
                if (cs && !cs->width.is_auto()) {
                    c->width = compute_content_width(c, cbw, viewport_w_, viewport_h_);
                } else {
                    float fe = c->margin[1] + c->margin[3] + c->border[1] + c->border[3] + c->padding[1] + c->padding[3];
                    auto [imn, imx] = intrinsic_widths(c, measurer_);   // shrink-to-fit = max-content
                    (void)imn;
                    c->width = std::max(0.0f, std::min(cbw - fe, imx - fe));
                    if (cs && !cs->max_width.is_auto()) c->width = std::min(c->width, std::max(0.0f, cs->max_width.resolve(fs, cbw, 16.0f, viewport_w_, viewport_h_)));
                }
                layout_box_contents(c);  // pass 1: size (for right/bottom anchoring)
                bool hl = cs && !cs->left.is_auto(), hr = cs && !cs->right.is_auto();
                bool ht = cs && !cs->top.is_auto(),  hb = cs && !cs->bottom.is_auto();
                float bx = hl ? cbx + offw(cs->left)
                         : hr ? cbx + cbw - offw(cs->right) - c->border_box_width() - c->margin[1]
                         : left_edge(cursor_y);                              // static x
                float by = ht ? cby + offh(cs->top)
                         : hb ? cby + cbh - offh(cs->bottom) - c->border_box_height() - c->margin[2]
                         : cursor_y;                                         // static y
                c->x = bx + c->margin[3] + c->border[3] + c->padding[3];
                c->y = by + c->margin[0] + c->border[0] + c->padding[0];
                layout_box_contents(c);  // pass 2: children follow final position
                ++i; continue;  // does not advance the in-flow cursor
            }

            if (flt != malibu::css::FloatType::None) {
                // Floated box: shrink-to-fit width (explicit width, table, or the
                // available band). Pass 1 finalizes width/height; then we place it
                // against the side edge and re-lay-out so children follow.
                float avail = right_edge(cursor_y) - left_edge(cursor_y);
                if (cs && !cs->width.is_auto()) {
                    c->width = compute_content_width(c, box->width, viewport_w_, viewport_h_);
                } else if (c->type == BoxType::Table) {
                    c->width = std::max(0.0f, std::min(avail, box->width));   // table sizes itself
                } else {
                    float fe = c->margin[1] + c->margin[3] + c->border[1] + c->border[3] + c->padding[1] + c->padding[3];
                    auto [imn, imx] = intrinsic_widths(c, measurer_);        // shrink-to-fit = max-content, clamped
                    c->width = std::max(0.0f, std::min(avail - fe, imx - fe));
                    if (cs && !cs->max_width.is_auto()) c->width = std::min(c->width, std::max(0.0f, cs->max_width.resolve(c->style->font_size, avail, 16.0f, viewport_w_, viewport_h_)));
                }
                layout_box_contents(c);                          // pass 1: finalize size
                float mbw = c->margin_box_width();
                float fy = cursor_y;
                if (mbw > right_edge(fy) - left_edge(fy)) clear_to(malibu::css::ClearType::Both, fy);
                float inner;
                if (flt == malibu::css::FloatType::Left) {
                    float le = left_edge(fy);
                    c->x = le + c->margin[3] + c->border[3] + c->padding[3];
                    inner = le + mbw;
                } else {
                    float re = right_edge(fy);
                    c->x = re - mbw + c->margin[3] + c->border[3] + c->padding[3];
                    inner = re - mbw;
                }
                c->y = fy + c->margin[0] + c->border[0] + c->padding[0];
                layout_box_contents(c);                          // pass 2: place children at final x/y
                floats_.push_back({flt, fy, fy + c->margin_box_height(), inner});
                ++i; continue;  // floats don't advance the normal-flow cursor
            }

            if (cs && cs->clear != malibu::css::ClearType::None) clear_to(cs->clear, cursor_y);

            // Normal in-flow block: fit between current float edges.
            // Adjacent vertical margins collapse: pull up by the overlap of the
            // previous block's bottom margin and this block's top margin.
            cursor_y -= std::min(prev_mb, c->margin[0]);
            float l = left_edge(cursor_y), r = right_edge(cursor_y);
            float avail = std::max(0.0f, r - l);
            c->width = compute_content_width(c, avail, viewport_w_, viewport_h_);
            float rel_x = 0, rel_y = 0;
            if (pos == malibu::css::PositionType::Relative && cs) { rel_x = off(cs->left); rel_y = off(cs->top); }
            c->x = l + rel_x + c->margin[3] + c->border[3] + c->padding[3];
            c->y = cursor_y + rel_y + c->margin[0] + c->border[0] + c->padding[0];
            layout_box_contents(c);
            cursor_y += c->margin_box_height();
            prev_mb = c->margin[2];
            ++i;
        }
    }
    // Enclose this block's own floats; then pop them and restore the BFC scope.
    for (size_t k = my_start; k < floats_.size(); ++k) cursor_y = std::max(cursor_y, floats_[k].bottom);
    floats_.resize(my_start);
    bfc_base_ = saved_bfc;
    return cursor_y - box->y;
}

namespace {
// Estimate a box's single-line content width (sum of glyph advances of its
// descendant text) — the basis for auto table-column sizing.
float natural_text_width(LayoutBox* b, const layout::TextMeasurer* m) {
    float w = 0;
    if (b->type == BoxType::Text) {
        float fs = (b->parent && b->parent->style) ? b->parent->style->font_size : 16.0f;
        for (char16_t ch : b->text) w += m ? m->char_advance(ch, fs) : fs * 0.5f;
        return w;
    }
    for (LayoutBox* c : b->children) w += natural_text_width(c, m);
    return w;
}

std::pair<float, float> intrinsic_widths(LayoutBox* b, const layout::TextMeasurer* m) {
    if (b->type == BoxType::Text) {
        float fs = (b->parent && b->parent->style) ? b->parent->style->font_size : 16.0f;
        float total = 0, longest = 0, cur = 0;
        for (char16_t ch : b->text) {
            float a = m ? m->char_advance(ch, fs) : fs * 0.5f;
            total += a;
            if (ch == u' ' || ch == u'\t' || ch == u'\n' || ch == u'\r') { longest = std::max(longest, cur); cur = 0; }
            else cur += a;
        }
        longest = std::max(longest, cur);
        return {longest, total};   // min-content = widest word, max-content = whole line
    }
    const ComputedStyle* s = b->style;
    float edges = b->margin[1] + b->margin[3] + b->border[1] + b->border[3] + b->padding[1] + b->padding[3];
    if (s && !s->width.is_auto() && !s->width.is_percent()) {
        float w = s->width.resolve(s->font_size, 0, 16.0f, 0, 0);
        return {w + edges, w + edges};   // explicit width fixes both
    }
    if (b->is_replaced) {
        float w = b->intrinsic_width;
        const float ratio = replaced_ratio(b);
        if (s && !s->height.is_auto() && !s->height.is_percent() && ratio > 0)
            w = s->height.resolve(s->font_size, 0, 16.0f, 0, 0) * ratio;
        return {w + edges, w + edges};
    }
    float cmin = 0, cmax = 0;
    for (LayoutBox* c : b->children) {
        auto [cm, cM] = intrinsic_widths(c, m);
        cmin = std::max(cmin, cm);
        if (is_inline_level(c->type)) cmax += cM; else cmax = std::max(cmax, cM);  // inline sums, block stacks
    }
    return {cmin + edges, cmax + edges};
}
}  // namespace

namespace {
// Parses a grid-template-columns track list into resolved column widths for a
// container of `avail` content width with `gap` between tracks. Handles px / %
// / fr / auto / repeat(n, <track>). `fr` shares the leftover space.
std::vector<float> parse_grid_tracks(const std::u16string& spec, float avail, float gap, float fs) {
    // Tokenize, expanding repeat(n, tok...).
    std::vector<std::u16string> toks;
    for (size_t i = 0; i < spec.size();) {
        while (i < spec.size() && (spec[i] == u' ' || spec[i] == u'\t')) ++i;
        if (i >= spec.size()) break;
        if (spec.compare(i, 7, u"repeat(") == 0) {
            size_t close = spec.find(u')', i); std::u16string inner = spec.substr(i + 7, (close == std::u16string::npos ? spec.size() : close) - (i + 7));
            i = (close == std::u16string::npos) ? spec.size() : close + 1;
            size_t comma = inner.find(u','); if (comma == std::u16string::npos) continue;
            int n = std::atoi(std::string(inner.begin(), inner.begin() + comma).c_str());
            std::u16string body = inner.substr(comma + 1);
            // collect track tokens inside the repeat body
            std::vector<std::u16string> bt; size_t j = 0;
            while (j < body.size()) { while (j < body.size() && body[j] == u' ') ++j; size_t st = j; while (j < body.size() && body[j] != u' ') ++j; if (j > st) bt.push_back(body.substr(st, j - st)); }
            for (int r = 0; r < std::max(1, n); ++r) for (auto& t : bt) toks.push_back(t);
        } else {
            size_t st = i; while (i < spec.size() && spec[i] != u' ') ++i; toks.push_back(spec.substr(st, i - st));
        }
    }
    if (toks.empty()) return {};
    std::vector<float> w(toks.size(), 0); std::vector<float> fr(toks.size(), 0);
    float used = gap * (toks.size() - 1); float frsum = 0;
    for (size_t i = 0; i < toks.size(); ++i) {
        const std::u16string& t = toks[i];
        if (t.size() > 2 && t.compare(t.size() - 2, 2, u"fr") == 0) { fr[i] = std::atof(std::string(t.begin(), t.end() - 2).c_str()); frsum += fr[i]; }
        else if (t.size() > 1 && t.back() == u'%') { w[i] = std::atof(std::string(t.begin(), t.end() - 1).c_str()) / 100.0f * avail; used += w[i]; }
        else if (t == u"auto") { fr[i] = 1; frsum += 1; }   // approximate auto as 1fr
        else { w[i] = std::atof(std::string(t.begin(), t.end()).c_str()); if (t.find(u"em") != std::u16string::npos) w[i] *= fs; used += w[i]; }
    }
    float leftover = std::max(0.0f, avail - used);
    for (size_t i = 0; i < toks.size(); ++i) if (fr[i] > 0 && frsum > 0) w[i] = leftover * fr[i] / frsum;
    return w;
}
}  // namespace

void LayoutEngine::layout_grid(LayoutBox* box) {
    float fs = box->style ? box->style->font_size : 16.0f;
    float gap = box->style ? box->style->gap : 0.0f;
    std::vector<float> cols = parse_grid_tracks(box->style ? box->style->grid_template_columns : u"", box->width, gap, fs);
    if (cols.empty()) { float chh = layout_children(box); box->height = compute_height(box, chh, viewport_w_, viewport_h_); return; }

    std::vector<LayoutBox*> items;
    for (LayoutBox* c : box->children) if (c->type != BoxType::Text || !c->text.empty()) items.push_back(c);

    float y = box->y, rowh = 0, x = box->x; size_t col = 0;
    for (LayoutBox* it : items) {
        if (col >= cols.size()) { col = 0; x = box->x; y += rowh + gap; rowh = 0; }
        resolve_edges(it, cols[col]);
        it->width = std::max(0.0f, cols[col] - (it->margin[1] + it->margin[3] + it->border[1] + it->border[3] + it->padding[1] + it->padding[3]));
        it->x = x + it->margin[3] + it->border[3] + it->padding[3];
        it->y = y + it->margin[0] + it->border[0] + it->padding[0];
        layout_box_contents(it);
        rowh = std::max(rowh, it->margin_box_height());
        x += cols[col] + gap;
        ++col;
    }
    box->height = (items.empty() ? 0 : (y + rowh - box->y));
    if (box->style && !box->style->height.is_auto())
        box->height = compute_height(box, box->height, viewport_w_, viewport_h_);
}

void LayoutEngine::layout_table(LayoutBox* box) {
    // Collect rows (flattening row groups: tbody/thead/tfoot).
    std::vector<LayoutBox*> rows;
    std::function<void(LayoutBox*)> collect = [&](LayoutBox* b) {
        for (LayoutBox* c : b->children) {
            if (c->type == BoxType::TableRow) rows.push_back(c);
            else if (c->type == BoxType::TableRowGroup) collect(c);
        }
    };
    collect(box);
    if (rows.empty()) { box->height = compute_height(box, 0.0f, viewport_w_, viewport_h_); return; }

    size_t ncols = 0;
    for (auto* r : rows) {
        size_t cells = 0;
        for (LayoutBox* c : r->children) if (c->type == BoxType::TableCell) cells++;
        ncols = std::max(ncols, cells);
    }
    if (ncols == 0) { box->height = compute_height(box, 0.0f, viewport_w_, viewport_h_); return; }

    float table_w = box->width;  // content width (already computed by the caller)

    // Auto column widths from intrinsic sizing: column max = widest cell
    // max-content, column min = widest cell min-content (so shrinking can't go
    // below the longest word).
    std::vector<float> colw(ncols, 0.0f), colmin(ncols, 0.0f);
    for (auto* r : rows) {
        size_t ci = 0;
        for (LayoutBox* cell : r->children) {
            if (cell->type != BoxType::TableCell) continue;
            resolve_edges(cell, table_w);
            float pad = cell->padding[1] + cell->padding[3] + cell->border[1] + cell->border[3] + 2.0f;
            auto [cmn, cmx] = intrinsic_widths(cell, measurer_);
            float maxw = cmx + pad, minw = cmn + pad;
            if (cell->style && !cell->style->width.is_auto())
                maxw = minw = cell->style->width.resolve(cell->style->font_size, table_w, 16.0f, viewport_w_, viewport_h_) + pad;
            colw[ci] = std::max(colw[ci], maxw);
            colmin[ci] = std::max(colmin[ci], minw);
            ci++;
        }
    }
    float total = 0; for (float w : colw) total += w;
    if (total <= 0) total = 1;
    bool definite = box->style && !box->style->width.is_auto();
    if (total > table_w && table_w > 0) {                 // overflow: shrink columns to min, floored
        float min_total = 0; for (float m : colmin) min_total += m;
        float shrinkable = total - min_total, need = total - table_w;
        if (shrinkable > 0 && need > 0) {
            float k = std::min(1.0f, need / shrinkable);
            for (size_t i = 0; i < ncols; ++i) colw[i] -= (colw[i] - colmin[i]) * k;
        }
    } else if (definite && total < table_w) {             // explicit width: distribute slack
        float extra = (table_w - total) / ncols; for (auto& w : colw) w += extra;
    } else {
        table_w = total;                                  // auto width: shrink to content
    }

    // Position cells in the grid; row height = tallest cell.
    float y = box->y;
    for (auto* r : rows) {
        float x = box->x, rowh = 0;
        std::vector<LayoutBox*> cells;
        for (LayoutBox* c : r->children) if (c->type == BoxType::TableCell) cells.push_back(c);
        for (size_t k = 0; k < cells.size() && k < ncols; ++k) {
            LayoutBox* cell = cells[k];
            float inner = colw[k] - (cell->padding[1] + cell->padding[3] + cell->border[1] + cell->border[3]);
            cell->width = std::max(0.0f, inner);
            cell->x = x + cell->margin[3] + cell->border[3] + cell->padding[3];
            cell->y = y + cell->margin[0] + cell->border[0] + cell->padding[0];
            float chh = layout_children(cell);
            cell->height = compute_height(cell, chh, viewport_w_, viewport_h_);
            rowh = std::max(rowh, cell->border_box_height());
            x += colw[k];
        }
        // Stretch every cell to the row height (table-cell vertical fill).
        for (LayoutBox* cell : cells)
            cell->height = std::max(cell->height, rowh - (cell->padding[0] + cell->padding[2] + cell->border[0] + cell->border[2]));
        r->x = box->x; r->y = y; r->width = table_w; r->height = rowh;
        y += rowh;
    }
    box->width = table_w;
    box->height = y - box->y;
}

LayoutBox* LayoutEngine::layout_document(Document& doc, float viewport_width, float viewport_height) {
    pool_.clear();
    node_to_box_.clear();
    viewport_w_ = viewport_width;
    viewport_h_ = viewport_height;
    doc_ = &doc;

    root_ = alloc_box();
    root_->type = BoxType::Block;
    root_->style = nullptr;
    root_->x = 0; root_->y = 0;
    root_->width = viewport_width;

    if (const NodeCore* r = doc.core(doc.root())) {
        for (malibu::NodeHandle child : r->children) {
            if (LayoutBox* cb = build_box(doc, child)) {
                cb->parent = root_;
                root_->children.push_back(cb);
            }
        }
    }
    floats_.clear(); bfc_base_ = 0;
    float h = layout_children(root_);
    root_->height = std::max(h, viewport_height);
    needs_layout_ = false;

    // MALIBU_LAYOUT_DEBUG: dump the box-tree width chain (tag#id.class x w type)
    // to diagnose where a layout collapses. Limited depth/count.
    if (std::getenv("MALIBU_LAYOUT_DEBUG")) {
        int count = 0;
        std::function<void(LayoutBox*, int)> dump = [&](LayoutBox* b, int depth) {
            if (count++ > 400 || depth > 10) return;
            if (b->style && b->node.index) {
                const NodeCore* c = doc.core(b->node);
                std::string tag = c ? std::string(c->tag_name.begin(), c->tag_name.end()) : "?";
                std::string id, cls;
                if (c) for (auto& [k, v] : c->attributes) {
                    if (k == u"id") id = "#" + std::string(v.begin(), v.end());
                    else if (k == u"class") cls = "." + std::string(v.begin(), v.end()).substr(0, 24);
                }
                std::fprintf(stderr, "%*s%s%s%s  x=%.0f w=%.0f h=%.0f t=%d color=%d,%d,%d,%d vis=%d op=%.2f\n", depth * 2, "",
                             tag.c_str(), id.c_str(), cls.c_str(), b->x, b->width, b->height, (int)b->type,
                             b->style->color.r, b->style->color.g, b->style->color.b, b->style->color.a,
                             (int)b->style->visibility, b->style->opacity);
            }
            for (LayoutBox* ch : b->children) dump(ch, depth + 1);
        };
        dump(root_, 0);
    }
    return root_;
}

void LayoutEngine::mark_dirty(malibu::NodeHandle h) {
    needs_layout_ = true;
    if (LayoutBox* b = box_for_node(h)) b->is_dirty = true;
}

void LayoutEngine::incremental_layout(Document& doc) {
    if (needs_layout_) layout_document(doc, viewport_w_, viewport_h_);
}

void LayoutEngine::force_layout_for_read(Document& doc) {
    if (needs_layout_) layout_document(doc, viewport_w_, viewport_h_);
}

} // namespace malibu::layout
