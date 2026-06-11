#pragma once
// core/include/malibu/canvas/canvas2d.h
// MalibuCanvas — a from-scratch immediate-mode 2D raster surface (the engine
// behind <canvas>.getContext("2d")). No JS/DOM dependency: it is a pure RGBA
// bitmap plus drawing operations. The host blits the bitmap into the page.

#include <cstdint>
#include <string>
#include <vector>

namespace malibu::canvas {

struct RGBA { uint8_t r = 0, g = 0, b = 0, a = 255; };

// Parses a CSS color string (#rgb, #rrggbb, rgb()/rgba(), basic names).
RGBA parse_color(const std::string& s);

class Canvas2D {
public:
    Canvas2D(int w, int h) { resize(w, h); }

    int  width() const { return w_; }
    int  height() const { return h_; }
    const std::vector<uint8_t>& pixels() const { return px_; }  // RGBA, row-major
    void resize(int w, int h);

    // ---- state ----
    void set_fill_style(const std::string& c) { fill_ = parse_color(c); }
    void set_stroke_style(const std::string& c) { stroke_ = parse_color(c); }
    void set_line_width(double w) { line_width_ = w <= 0 ? 1.0 : w; }
    void set_global_alpha(double a) { global_alpha_ = a < 0 ? 0 : (a > 1 ? 1 : a); }

    // ---- rectangles ----
    void clear_rect(double x, double y, double w, double h);
    void fill_rect(double x, double y, double w, double h);
    void stroke_rect(double x, double y, double w, double h);

    // ---- paths ----
    void begin_path() { subpaths_.clear(); subpaths_.emplace_back(); }
    void close_path();
    void move_to(double x, double y);
    void line_to(double x, double y);
    void arc(double cx, double cy, double r, double a0, double a1, bool ccw);
    void rect(double x, double y, double w, double h);
    void fill();
    void stroke();

    // ---- direct pixel access (ImageData) ----
    void put_pixel(int x, int y, RGBA c);

private:
    struct Pt { double x, y; };
    void blend(int x, int y, RGBA c, double alpha);
    void fill_polygon(const std::vector<Pt>& poly, RGBA c);
    void draw_line(double x0, double y0, double x1, double y1, RGBA c, double width);

    int w_ = 0, h_ = 0;
    std::vector<uint8_t> px_;
    RGBA fill_{0, 0, 0, 255};
    RGBA stroke_{0, 0, 0, 255};
    double line_width_ = 1.0;
    double global_alpha_ = 1.0;
    std::vector<std::vector<Pt>> subpaths_{{}};
};

} // namespace malibu::canvas
