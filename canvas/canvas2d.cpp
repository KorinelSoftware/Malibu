// canvas/canvas2d.cpp — MalibuCanvas 2D raster surface.
#include "malibu/canvas/canvas2d.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace malibu::canvas {
namespace {
int hex1(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}
std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower((unsigned char)c)); return s; }
}  // namespace

RGBA parse_color(const std::string& in) {
    std::string s = in;
    // trim
    size_t a = s.find_first_not_of(" \t"); size_t b = s.find_last_not_of(" \t");
    if (a == std::string::npos) return {0, 0, 0, 255};
    s = s.substr(a, b - a + 1);
    std::string low = lower(s);

    if (!s.empty() && s[0] == '#') {
        std::string h = s.substr(1);
        if (h.size() == 3) return {static_cast<uint8_t>(hex1(h[0]) * 17), static_cast<uint8_t>(hex1(h[1]) * 17), static_cast<uint8_t>(hex1(h[2]) * 17), 255};
        if (h.size() == 6) return {static_cast<uint8_t>(hex1(h[0]) * 16 + hex1(h[1])), static_cast<uint8_t>(hex1(h[2]) * 16 + hex1(h[3])), static_cast<uint8_t>(hex1(h[4]) * 16 + hex1(h[5])), 255};
    }
    if (low.rfind("rgb", 0) == 0) {
        size_t lp = s.find('('); size_t rp = s.find(')');
        if (lp != std::string::npos && rp != std::string::npos) {
            std::string body = s.substr(lp + 1, rp - lp - 1);
            double v[4] = {0, 0, 0, 1};
            int n = 0; size_t pos = 0;
            while (n < 4 && pos < body.size()) {
                size_t comma = body.find(',', pos);
                std::string tok = body.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                v[n++] = std::atof(tok.c_str());
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            return {static_cast<uint8_t>(std::clamp(v[0], 0.0, 255.0)),
                    static_cast<uint8_t>(std::clamp(v[1], 0.0, 255.0)),
                    static_cast<uint8_t>(std::clamp(v[2], 0.0, 255.0)),
                    static_cast<uint8_t>(std::clamp(v[3] * 255.0, 0.0, 255.0))};
        }
    }
    struct Named { const char* n; RGBA c; };
    static const Named names[] = {
        {"black", {0,0,0,255}}, {"white", {255,255,255,255}}, {"red", {255,0,0,255}},
        {"green", {0,128,0,255}}, {"blue", {0,0,255,255}}, {"yellow", {255,255,0,255}},
        {"cyan", {0,255,255,255}}, {"magenta", {255,0,255,255}}, {"gray", {128,128,128,255}},
        {"grey", {128,128,128,255}}, {"orange", {255,165,0,255}}, {"purple", {128,0,128,255}},
        {"transparent", {0,0,0,0}},
    };
    for (auto& nm : names) if (low == nm.n) return nm.c;
    return {0, 0, 0, 255};
}

void Canvas2D::resize(int w, int h) {
    w_ = std::max(0, w); h_ = std::max(0, h);
    px_.assign(static_cast<size_t>(w_) * h_ * 4, 0);  // transparent black
}

void Canvas2D::blend(int x, int y, RGBA c, double alpha) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    double sa = (c.a / 255.0) * alpha * global_alpha_;
    if (sa <= 0) return;
    size_t i = (static_cast<size_t>(y) * w_ + x) * 4;
    double da = px_[i + 3] / 255.0;
    double oa = sa + da * (1 - sa);
    auto ch = [&](int k, uint8_t sc) {
        double dc = px_[i + k] / 255.0;
        double oc = (sc / 255.0 * sa + dc * da * (1 - sa)) / (oa <= 0 ? 1 : oa);
        px_[i + k] = static_cast<uint8_t>(std::clamp(oc * 255.0, 0.0, 255.0));
    };
    ch(0, c.r); ch(1, c.g); ch(2, c.b);
    px_[i + 3] = static_cast<uint8_t>(std::clamp(oa * 255.0, 0.0, 255.0));
}

void Canvas2D::put_pixel(int x, int y, RGBA c) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    size_t i = (static_cast<size_t>(y) * w_ + x) * 4;
    px_[i] = c.r; px_[i + 1] = c.g; px_[i + 2] = c.b; px_[i + 3] = c.a;
}

void Canvas2D::clear_rect(double x, double y, double w, double h) {
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    int x1 = (int)std::ceil(x + w), y1 = (int)std::ceil(y + h);
    for (int yy = std::max(0, y0); yy < std::min(h_, y1); ++yy)
        for (int xx = std::max(0, x0); xx < std::min(w_, x1); ++xx) {
            size_t i = (static_cast<size_t>(yy) * w_ + xx) * 4;
            px_[i] = px_[i + 1] = px_[i + 2] = px_[i + 3] = 0;
        }
}

void Canvas2D::fill_rect(double x, double y, double w, double h) {
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    int x1 = (int)std::ceil(x + w), y1 = (int)std::ceil(y + h);
    for (int yy = std::max(0, y0); yy < std::min(h_, y1); ++yy)
        for (int xx = std::max(0, x0); xx < std::min(w_, x1); ++xx)
            blend(xx, yy, fill_, 1.0);
}

void Canvas2D::stroke_rect(double x, double y, double w, double h) {
    draw_line(x, y, x + w, y, stroke_, line_width_);
    draw_line(x + w, y, x + w, y + h, stroke_, line_width_);
    draw_line(x + w, y + h, x, y + h, stroke_, line_width_);
    draw_line(x, y + h, x, y, stroke_, line_width_);
}

void Canvas2D::move_to(double x, double y) {
    if (subpaths_.empty()) subpaths_.emplace_back();
    if (!subpaths_.back().empty()) subpaths_.emplace_back();
    subpaths_.back().push_back({x, y});
}
void Canvas2D::line_to(double x, double y) {
    if (subpaths_.empty() || subpaths_.back().empty()) { move_to(x, y); return; }
    subpaths_.back().push_back({x, y});
}
void Canvas2D::close_path() {
    if (!subpaths_.empty() && subpaths_.back().size() > 1)
        subpaths_.back().push_back(subpaths_.back().front());
}
void Canvas2D::rect(double x, double y, double w, double h) {
    move_to(x, y); line_to(x + w, y); line_to(x + w, y + h); line_to(x, y + h); close_path();
}
void Canvas2D::arc(double cx, double cy, double r, double a0, double a1, bool ccw) {
    const int segs = std::max(8, (int)(std::abs(a1 - a0) * r / 3));
    if (ccw && a1 > a0) a1 -= 2 * M_PI;
    if (!ccw && a1 < a0) a1 += 2 * M_PI;
    for (int i = 0; i <= segs; ++i) {
        double t = a0 + (a1 - a0) * (double)i / segs;
        double x = cx + std::cos(t) * r, y = cy + std::sin(t) * r;
        if (i == 0 && (subpaths_.empty() || subpaths_.back().empty())) move_to(x, y);
        else line_to(x, y);
    }
}

void Canvas2D::fill_polygon(const std::vector<Pt>& poly, RGBA c) {
    if (poly.size() < 3) return;
    double miny = poly[0].y, maxy = poly[0].y;
    for (auto& p : poly) { miny = std::min(miny, p.y); maxy = std::max(maxy, p.y); }
    int y0 = std::max(0, (int)std::floor(miny)), y1 = std::min(h_ - 1, (int)std::ceil(maxy));
    for (int y = y0; y <= y1; ++y) {
        double sy = y + 0.5;
        std::vector<double> xs;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            double ay = poly[i].y, by = poly[j].y;
            if ((ay <= sy && by > sy) || (by <= sy && ay > sy)) {
                double t = (sy - ay) / (by - ay);
                xs.push_back(poly[i].x + t * (poly[j].x - poly[i].x));
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2) {
            int xa = std::max(0, (int)std::ceil(xs[i] - 0.5));
            int xb = std::min(w_ - 1, (int)std::floor(xs[i + 1] - 0.5));
            for (int x = xa; x <= xb; ++x) blend(x, y, c, 1.0);
        }
    }
}

void Canvas2D::fill() {
    for (auto& sp : subpaths_) if (sp.size() >= 3) fill_polygon(sp, fill_);
}

void Canvas2D::draw_line(double x0, double y0, double x1, double y1, RGBA c, double width) {
    // Thick line as a quad (perpendicular offset).
    double dx = x1 - x0, dy = y1 - y0;
    double len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6) { fill_polygon({{x0 - width/2, y0 - width/2}, {x0 + width/2, y0 - width/2}, {x0 + width/2, y0 + width/2}, {x0 - width/2, y0 + width/2}}, c); return; }
    double nx = -dy / len * (width / 2), ny = dx / len * (width / 2);
    std::vector<Pt> quad = {{x0 + nx, y0 + ny}, {x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}, {x0 - nx, y0 - ny}};
    fill_polygon(quad, c);
}

void Canvas2D::stroke() {
    for (auto& sp : subpaths_)
        for (size_t i = 0; i + 1 < sp.size(); ++i)
            draw_line(sp[i].x, sp[i].y, sp[i + 1].x, sp[i + 1].y, stroke_, line_width_);
}

} // namespace malibu::canvas
