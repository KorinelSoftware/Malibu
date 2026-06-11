// render/raster/software_rasterizer.cpp
// CPU compositor for the display list.

#include "malibu/render/raster/software_rasterizer.h"

#include <algorithm>
#include <cmath>

namespace malibu::render {
namespace {

struct Rect { float x0, y0, x1, y1; };

void transform_point(const Transform2D& t, float x, float y, float& ox, float& oy) {
    ox = t.a * x + t.c * y + t.e;
    oy = t.b * x + t.d * y + t.f;
}

// Axis-aligned bounding box of a transformed rectangle.
Rect transformed_bbox(const Transform2D& t, float x, float y, float w, float h) {
    float xs[4], ys[4];
    transform_point(t, x, y, xs[0], ys[0]);
    transform_point(t, x + w, y, xs[1], ys[1]);
    transform_point(t, x, y + h, xs[2], ys[2]);
    transform_point(t, x + w, y + h, xs[3], ys[3]);
    Rect r{xs[0], ys[0], xs[0], ys[0]};
    for (int i = 1; i < 4; ++i) {
        r.x0 = std::min(r.x0, xs[i]); r.y0 = std::min(r.y0, ys[i]);
        r.x1 = std::max(r.x1, xs[i]); r.y1 = std::max(r.y1, ys[i]);
    }
    return r;
}

void blend_pixel(Framebuffer& fb, int x, int y, Color src, float alpha) {
    if (x < 0 || y < 0 || x >= fb.width || y >= fb.height) return;
    float a = (src.a / 255.0f) * alpha;
    if (a <= 0.0f) return;
    size_t i = (static_cast<size_t>(y) * fb.width + x) * 4;
    float inv = 1.0f - a;
    fb.rgba[i + 0] = static_cast<uint8_t>(src.r * a + fb.rgba[i + 0] * inv);
    fb.rgba[i + 1] = static_cast<uint8_t>(src.g * a + fb.rgba[i + 1] * inv);
    fb.rgba[i + 2] = static_cast<uint8_t>(src.b * a + fb.rgba[i + 2] * inv);
    fb.rgba[i + 3] = static_cast<uint8_t>(std::min(255.0f, src.a * alpha + fb.rgba[i + 3] * inv));
}

void fill_rect(Framebuffer& fb, Rect r, Color c, float alpha, const ClipRect& clip) {
    float x0 = r.x0, y0 = r.y0, x1 = r.x1, y1 = r.y1;
    if (clip.active) {
        x0 = std::max(x0, clip.x);          y0 = std::max(y0, clip.y);
        x1 = std::min(x1, clip.x + clip.w); y1 = std::min(y1, clip.y + clip.h);
    }
    int ix0 = std::max(0, static_cast<int>(std::floor(x0)));
    int iy0 = std::max(0, static_cast<int>(std::floor(y0)));
    int ix1 = std::min(fb.width, static_cast<int>(std::ceil(x1)));
    int iy1 = std::min(fb.height, static_cast<int>(std::ceil(y1)));
    for (int y = iy0; y < iy1; ++y)
        for (int x = ix0; x < ix1; ++x)
            blend_pixel(fb, x, y, c, alpha);
}

Color lerp_color(Color a, Color b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {(uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
            (uint8_t)(a.b + (b.b - a.b) * t), (uint8_t)(a.a + (b.a - a.a) * t)};
}
Color sample_gradient(const PaintRect& pr, float t) {
    const auto& st = pr.grad_stops;
    if (t <= st.front().pos) return st.front().color;
    if (t >= st.back().pos)  return st.back().color;
    for (size_t i = 1; i < st.size(); ++i)
        if (t <= st[i].pos) {
            float span = st[i].pos - st[i-1].pos;
            float lt = span > 0 ? (t - st[i-1].pos) / span : 0.0f;
            return lerp_color(st[i-1].color, st[i].color, lt);
        }
    return st.back().color;
}

// Fills a box's background honoring gradient, border-radius and (soft) shadow.
// Assumes translate/scale transforms (no rotation), so the transformed bbox maps
// 1:1 to the local rect — corner rounding / gradient use the normalized position.
void fill_box(Framebuffer& fb, Rect bb, const PaintRect& pr, float opacity, const ClipRect& clip) {
    float bx0 = bb.x0, by0 = bb.y0, bx1 = bb.x1, by1 = bb.y1;
    float W = bx1 - bx0, H = by1 - by0;
    if (W <= 0 || H <= 0) return;
    float rad = pr.border_radius; rad = std::min(rad, std::min(W, H) * 0.5f);
    // gradient axis projection bounds
    float ar = pr.grad_angle * 3.14159265f / 180.0f, ux = std::sin(ar), uy = -std::cos(ar);
    float pmin = std::min(std::min(0.0f, ux), std::min(uy, ux + uy));
    float pmax = std::max(std::max(0.0f, ux), std::max(uy, ux + uy));
    float pspan = (pmax - pmin) > 1e-4f ? (pmax - pmin) : 1.0f;

    float cx0 = bx0, cy0 = by0, cx1 = bx1, cy1 = by1;
    if (clip.active) { cx0 = std::max(cx0, clip.x); cy0 = std::max(cy0, clip.y); cx1 = std::min(cx1, clip.x + clip.w); cy1 = std::min(cy1, clip.y + clip.h); }
    int ix0 = std::max(0, (int)std::floor(cx0)), iy0 = std::max(0, (int)std::floor(cy0));
    int ix1 = std::min(fb.width, (int)std::ceil(cx1)), iy1 = std::min(fb.height, (int)std::ceil(cy1));
    for (int y = iy0; y < iy1; ++y) {
        float ly = y + 0.5f - by0;
        for (int x = ix0; x < ix1; ++x) {
            float lx = x + 0.5f - bx0;
            float cov = 1.0f;
            if (rad > 0.5f) {  // rounded-corner coverage (1px AA)
                float ccx = lx < rad ? rad : (lx > W - rad ? W - rad : lx);
                float ccy = ly < rad ? rad : (ly > H - rad ? H - rad : ly);
                float dx = lx - ccx, dy = ly - ccy, d = std::sqrt(dx*dx + dy*dy);
                if (d > rad) cov = std::clamp(rad + 0.5f - d, 0.0f, 1.0f);
            }
            if (pr.shadow && pr.blur > 0.5f) {  // feather alpha toward the edges
                float edge = std::min(std::min(lx, W - lx), std::min(ly, H - ly));
                cov *= std::clamp(edge / pr.blur, 0.0f, 1.0f);
            }
            if (cov <= 0.0f) continue;
            Color col = pr.background;
            if (pr.gradient && pr.grad_stops.size() >= 2) {
                float t = ((lx / W) * ux + (ly / H) * uy - pmin) / pspan;
                col = sample_gradient(pr, t);
            }
            blend_pixel(fb, x, y, col, opacity * cov);
        }
    }
}

}  // namespace

void SoftwareRasterizer::rasterize(const DisplayList& list, Framebuffer& fb, TextDrawer* text_drawer) {
    for (const DisplayItem& item : list.items()) {
        if (item.kind == DisplayItem::Kind::Rect) {
            const PaintRect& pr = item.rect;
            // Background fill: gradient/rounded/shadow path when needed, else fast solid.
            if (pr.shadow || pr.gradient || pr.border_radius > 0.5f) {
                if (pr.shadow || pr.gradient || !pr.background.transparent()) {
                    Rect bb = transformed_bbox(item.transform, pr.x, pr.y, pr.w, pr.h);
                    fill_box(fb, bb, pr, item.opacity, item.clip);
                }
            } else if (!pr.background.transparent()) {
                Rect bb = transformed_bbox(item.transform, pr.x, pr.y, pr.w, pr.h);
                fill_rect(fb, bb, pr.background, item.opacity, item.clip);
            }
            // Borders (four edge rects, per-side colors).
            {
                auto edge = [&](float x, float y, float w, float h, Color col) {
                    if (w <= 0 || h <= 0 || col.a == 0) return;
                    Rect bb = transformed_bbox(item.transform, x, y, w, h);
                    fill_rect(fb, bb, col, item.opacity, item.clip);
                };
                edge(pr.x, pr.y, pr.w, pr.border[0], pr.border_colors[0]);                          // top
                edge(pr.x + pr.w - pr.border[1], pr.y, pr.border[1], pr.h, pr.border_colors[1]);    // right
                edge(pr.x, pr.y + pr.h - pr.border[2], pr.w, pr.border[2], pr.border_colors[2]);    // bottom
                edge(pr.x, pr.y, pr.border[3], pr.h, pr.border_colors[3]);                          // left
            }
        } else {  // Text
            if (text_drawer) {
                text_drawer->draw_text(fb, item.text, item.transform, item.opacity, item.clip);
            } else {
                // Reference fallback: fill the run's bounds in the text color.
                const PaintText& pt = item.text;
                float w = 0;
                for (char16_t ch : pt.text) { (void)ch; w += pt.font_size * 0.5f; }
                Rect bb = transformed_bbox(item.transform, pt.x, pt.y, w, pt.font_size);
                fill_rect(fb, bb, pt.color, item.opacity * 0.85f, item.clip);
            }
        }
    }
}

} // namespace malibu::render
