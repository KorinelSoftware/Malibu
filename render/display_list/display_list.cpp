// render/display_list/display_list.cpp
#include <cstdio>
#include <cstdlib>
// Builds and sorts the display list from a laid-out box tree.

#include "malibu/render/display_list/display_list.h"
#include "malibu/layout/layout_box.h"
#include "malibu/dom/document.h"

#include <algorithm>

namespace malibu::render {

using malibu::layout::LayoutBox;
using malibu::layout::BoxType;
using malibu::css::ComputedStyle;

void DisplayList::sort() {
    std::stable_sort(items_.begin(), items_.end(),
                     [](const DisplayItem& a, const DisplayItem& b) {
                         if (a.z_index != b.z_index) return a.z_index < b.z_index;
                         return a.document_order < b.document_order;
                     });
}

namespace {

Transform2D mul(const Transform2D& m, const Transform2D& n) {
    return {m.a * n.a + m.c * n.b,       m.b * n.a + m.d * n.b,
            m.a * n.c + m.c * n.d,       m.b * n.c + m.d * n.d,
            m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f};
}

struct Ctx {
    DisplayList* list;
    uint32_t     order = 0;
};

void emit_box(Ctx& ctx, const LayoutBox* box, float opacity, const Transform2D& xform, ClipRect clip) {
    const ComputedStyle* s = box->style;
    float local_opacity = opacity * (s ? s->opacity : 1.0f);
    // position:fixed is pinned to the viewport (ignores the scroll translate in
    // `xform`); position:sticky scrolls normally but clamps so its top never goes
    // above `top` (the scroll offset lives in xform.f = -scroll_y).
    Transform2D base = xform;
    if (s && s->position == malibu::css::PositionType::Fixed) base = Transform2D{};
    else if (s && s->position == malibu::css::PositionType::Sticky) {
        float top = s->top.is_auto() ? 0.0f : s->top.resolve(s->font_size, 0.0f, 16.0f, 0.0f, 0.0f);
        base.f = std::max(xform.f, top - box->y);   // pin at `top` once scrolled past
    }
    Transform2D local_xform = (s && !s->transform.is_identity()) ? mul(base, s->transform) : base;
    int32_t z = (s && s->has_z_index) ? s->z_index : 0;

    if (box->type != BoxType::Text && s) {
        bool visible = s->visibility == malibu::css::VisibilityType::Visible;
        bool has_border = box->border[0] > 0 || box->border[1] > 0 || box->border[2] > 0 || box->border[3] > 0;
        float bx = box->x - box->padding[3] - box->border[3];
        float by = box->y - box->padding[0] - box->border[0];
        float bw = box->border_box_width(), bh = box->border_box_height();
        const float border_radius = s->border_radius_percent > 0
            ? std::min(bw, bh) * s->border_radius_percent
            : s->border_radius;

        // Drop shadow (drawn first, behind the box).
        if (visible && s->has_box_shadow && s->shadow_color.a > 0) {
            DisplayItem sh;
            sh.kind = DisplayItem::Kind::Rect; sh.z_index = z; sh.document_order = ctx.order;
            sh.opacity = local_opacity; sh.transform = local_xform; sh.clip = clip;
            float sp = s->shadow_spread, bl = s->shadow_blur;
            sh.rect.x = bx + s->shadow_x - sp - bl; sh.rect.y = by + s->shadow_y - sp - bl;
            sh.rect.w = bw + 2 * (sp + bl);          sh.rect.h = bh + 2 * (sp + bl);
            sh.rect.background = s->shadow_color;
            sh.rect.border_radius = border_radius + sp + bl;
            sh.rect.shadow = true; sh.rect.blur = bl;
            ctx.list->add(std::move(sh));
        }
        if (visible && (!s->background_color.transparent() || has_border || s->bg_gradient)) {
            DisplayItem item;
            item.kind = DisplayItem::Kind::Rect;
            item.z_index = z;
            item.document_order = ctx.order;
            item.opacity = local_opacity;
            item.transform = local_xform;
            item.clip = clip;
            item.rect.x = bx; item.rect.y = by; item.rect.w = bw; item.rect.h = bh;
            item.rect.background = s->background_color;
            item.rect.border[0] = box->border[0]; item.rect.border[1] = box->border[1];
            item.rect.border[2] = box->border[2]; item.rect.border[3] = box->border[3];
            item.rect.border_color = s->border_color;
            for (int k = 0; k < 4; ++k) item.rect.border_colors[k] = s->border_colors[k];
            item.rect.border_radius = border_radius;
            if (s->bg_gradient) { item.rect.gradient = true; item.rect.grad_angle = s->bg_angle; item.rect.grad_stops = s->bg_stops; }
            ctx.list->add(std::move(item));
        }
    } else if (box->type == BoxType::Text && !box->text.empty()) {
        const ComputedStyle* ps = box->parent ? box->parent->style : nullptr;
        Color tcol = ps ? ps->color : Color{0, 0, 0, 255};
        float fs = ps ? ps->font_size : 16.0f;
        auto td = ps ? ps->text_decoration : malibu::css::TextDecoration::None;
        // One text item per wrapped line fragment (so long text wraps); fall back
        // to the whole run if the box wasn't flowed into fragments.
        auto emit_line = [&](const std::u16string& str, float x, float y, float w) {
            DisplayItem it;
            it.kind = DisplayItem::Kind::Text; it.z_index = z; it.document_order = ctx.order;
            it.opacity = local_opacity; it.transform = local_xform; it.clip = clip;
            it.text.x = x; it.text.y = y; it.text.text = str; it.text.color = tcol; it.text.font_size = fs;
            ctx.list->add(std::move(it));
            if (td != malibu::css::TextDecoration::None && w > 0) {
                DisplayItem ul;
                ul.kind = DisplayItem::Kind::Rect; ul.z_index = z; ul.document_order = ctx.order;
                ul.opacity = local_opacity; ul.transform = local_xform; ul.clip = clip;
                float yoff = (td == malibu::css::TextDecoration::Overline) ? 0.05f
                           : (td == malibu::css::TextDecoration::LineThrough) ? 0.55f : 0.92f;
                ul.rect.x = x; ul.rect.w = w; ul.rect.y = y + fs * yoff; ul.rect.h = std::max(1.0f, fs / 14.0f);
                ul.rect.background = tcol;
                ctx.list->add(std::move(ul));
            }
        };
        if (!box->fragments.empty())
            for (const auto& f : box->fragments) emit_line(f.text, f.x, f.y, f.w);
        else
            emit_line(box->text, box->x, box->y, box->width);
    }

    // List-item marker (bullet / number) in the left padding area.
    if (box->type == BoxType::ListItem && s && s->list_style != malibu::css::ListStyleType::None) {
        std::u16string marker;
        if (s->list_style == malibu::css::ListStyleType::Decimal) {
            int idx = 1;
            if (box->parent) for (const LayoutBox* sib : box->parent->children) { if (sib == box) break; if (sib->type == BoxType::ListItem) idx++; }
            std::string n = std::to_string(idx) + ".";
            marker.assign(n.begin(), n.end());
        } else if (s->list_style == malibu::css::ListStyleType::Circle) marker = u"◦";
        else if (s->list_style == malibu::css::ListStyleType::Square)   marker = u"▪";
        else                                                            marker = u"•";  // disc
        DisplayItem m;
        m.kind = DisplayItem::Kind::Text;
        m.z_index = z; m.document_order = ctx.order; m.opacity = local_opacity;
        m.transform = local_xform; m.clip = clip;
        m.text.x = box->x - s->font_size * 1.3f;
        m.text.y = box->y;
        m.text.text = marker;
        m.text.color = s->color;
        m.text.font_size = s->font_size;
        ctx.list->add(std::move(m));
    }
    ctx.order++;

    // Establish a clip for overflow:hidden. Offset into device space by the
    // current transform's translation so it tracks scroll (clips are tested in
    // device coordinates by the rasterizer).
    ClipRect child_clip = clip;
    if (s && s->overflow != malibu::css::OverflowType::Visible) {
        child_clip = ClipRect{true,
                              box->x - box->padding[3] + local_xform.e,
                              box->y - box->padding[0] + local_xform.f,
                              box->width + box->padding[1] + box->padding[3],
                              box->height + box->padding[0] + box->padding[2]};
    }
    for (const LayoutBox* child : box->children)
        emit_box(ctx, child, local_opacity, local_xform, child_clip);
}

}  // namespace

DisplayList DisplayListBuilder::build(malibu::dom::Document& /*doc*/, malibu::layout::LayoutBox* root,
                                      float scroll_y) {
    DisplayList list;
    if (!root) return list;
    Ctx ctx{&list, 0};
    ClipRect none;
    Transform2D scroll{1, 0, 0, 1, 0, -scroll_y};   // translate content up by scroll_y
    for (const LayoutBox* child : root->children) emit_box(ctx, child, 1.0f, scroll, none);
    list.sort();
    if (std::getenv("MALIBU_DL_DEBUG")) {
        int texts = 0, rects = 0, shown = 0;
        for (const auto& it : list.items()) {
            if (it.kind == DisplayItem::Kind::Text) {
                texts++;
                if (!it.text.text.empty() && shown < 400) {
                    std::string s; for (char16_t ch : it.text.text) if (s.size() < 30) s.push_back(ch < 128 ? (char)ch : '?');
                    std::fprintf(stderr, "TEXT @x=%.0f y=%.0f fs=%.1f op=%.2f clip[%d %.0f,%.0f %.0fx%.0f] col=%d,%d,%d \"%s\"\n",
                        it.text.x, it.text.y, it.text.font_size, it.opacity,
                        it.clip.active, it.clip.x, it.clip.y, it.clip.w, it.clip.h,
                        it.text.color.r, it.text.color.g, it.text.color.b, s.c_str());
                    shown++;
                }
            } else {
                rects++;
                const auto& r = it.rect;
                if (std::getenv("MALIBU_DL_RECTS") && r.w > 200 && r.h > 40 && r.background.a > 40 && !r.shadow) {
                    std::fprintf(stderr, "RECT @x=%.0f y=%.0f %.0fx%.0f bg=%d,%d,%d,%d grad=%d\n",
                        r.x + it.transform.e, r.y + it.transform.f, r.w, r.h,
                        r.background.r, r.background.g, r.background.b, r.background.a, r.gradient);
                }
            }
        }
        std::fprintf(stderr, "=== DL: %d text items, %d rect items ===\n", texts, rects);
    }
    return list;
}

} // namespace malibu::render
