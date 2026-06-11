// image/svg.cpp — a compact SVG rasterizer to RGBA8. Outside core/.
//
// Covers the common icon/logo subset: <svg viewBox|width|height>, <rect>,
// <circle>, <ellipse>, <line>, <polygon>, <polyline>, <path> (M/L/H/V/C/S/Q/T/Z,
// absolute+relative), nested <g> with translate()/scale() transforms, and fill/
// stroke with named/#hex/rgb() colors + opacity. Shapes are flattened to
// polygons and scanline-filled (even-odd) with light anti-aliasing. Vector → it
// needs a target pixel size (the <img>/<svg> box).
#include "malibu/image/png.h"   // shares DecodedImage

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace malibu::image {
namespace {

struct Col { float r = 0, g = 0, b = 0, a = 0; };   // 0..1, a=0 → none
struct Pt { float x, y; };
struct Mat { float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0; };  // [a c e; b d f]
Pt apply(const Mat& m, Pt p) { return {m.a * p.x + m.c * p.y + m.e, m.b * p.x + m.d * p.y + m.f}; }
Mat mul(const Mat& m, const Mat& n) {
    return {m.a*n.a + m.c*n.b, m.b*n.a + m.d*n.b, m.a*n.c + m.c*n.d,
            m.b*n.c + m.d*n.d, m.a*n.e + m.c*n.f + m.e, m.b*n.e + m.d*n.f + m.f};
}

std::string lower(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 32; return s; }

// Parse a CSS-ish color into Col. Empty/none → a=0.
Col parse_color(std::string v) {
    v = lower(v);
    size_t b = v.find_first_not_of(" \t\n\r"); if (b == std::string::npos) return {};
    size_t e = v.find_last_not_of(" \t\n\r"); v = v.substr(b, e - b + 1);
    if (v.empty() || v == "none" || v == "transparent") return {};
    auto from255 = [](float x) { return x / 255.0f; };
    if (v[0] == '#') {
        unsigned r = 0, g = 0, bb = 0;
        if (v.size() >= 7) { r = std::stoul(v.substr(1,2),0,16); g = std::stoul(v.substr(3,2),0,16); bb = std::stoul(v.substr(5,2),0,16); }
        else if (v.size() >= 4) { r = std::stoul(v.substr(1,1),0,16)*17; g = std::stoul(v.substr(2,1),0,16)*17; bb = std::stoul(v.substr(3,1),0,16)*17; }
        return {from255(r), from255(g), from255(bb), 1};
    }
    if (v.rfind("rgb", 0) == 0) {
        size_t op = v.find('('); if (op == std::string::npos) return {{}};
        float c[4] = {0,0,0,1}; int i = 0; size_t p = op + 1;
        while (p < v.size() && i < 4) {
            while (p < v.size() && (v[p]==' '||v[p]==',')) ++p;
            c[i++] = std::atof(v.c_str() + p);
            while (p < v.size() && v[p]!=',' && v[p]!=')') ++p;
        }
        return {from255(c[0]), from255(c[1]), from255(c[2]), i >= 4 ? c[3] : 1.0f};
    }
    struct NC { const char* n; uint8_t r, g, b; };
    static const NC named[] = {
        {"black",0,0,0},{"white",255,255,255},{"red",255,0,0},{"green",0,128,0},{"blue",0,0,255},
        {"gray",128,128,128},{"grey",128,128,128},{"silver",192,192,192},{"orange",255,165,0},
        {"yellow",255,255,0},{"purple",128,0,128},{"navy",0,0,128},{"teal",0,128,128},
        {"currentcolor",0,0,0},{"darkgray",169,169,169},{"lightgray",211,211,211},
    };
    for (auto& n : named) if (v == n.n) return {from255(n.r), from255(n.g), from255(n.b), 1};
    return {0, 0, 0, 1};  // unknown → black (visible)
}

// Extract attribute `name` from a tag's attribute span. Returns "" if absent.
std::string attr(const std::string& tag, const std::string& name) {
    // SVG embedded in text/html goes through HTML attribute-name
    // normalization in Malibu's DOM. Match names ASCII-case-insensitively so
    // camel-cased SVG attributes such as viewBox survive serialization.
    const std::string folded_tag = lower(tag);
    const std::string folded_name = lower(name);
    size_t p = 0;
    while ((p = folded_tag.find(folded_name, p)) != std::string::npos) {
        // must be a whole attribute name (preceded by space, followed by = or space)
        bool lhs_ok = p == 0 || tag[p-1] == ' ' || tag[p-1] == '\t' || tag[p-1] == '\n' || tag[p-1] == '<';
        size_t after = p + folded_name.size();
        size_t q = after; while (q < tag.size() && (tag[q]==' '||tag[q]=='\t'||tag[q]=='\n')) ++q;
        if (lhs_ok && q < tag.size() && tag[q] == '=') {
            q++; while (q < tag.size() && (tag[q]==' '||tag[q]=='"'||tag[q]=='\'')) ++q;
            size_t st = q;
            char quote = (after < tag.size()) ? 0 : 0;
            // value ends at matching quote or whitespace/>
            // find the opening quote we skipped
            // simpler: read until " ' or whitespace or >
            while (q < tag.size() && tag[q] != '"' && tag[q] != '\'' && tag[q] != '>' ) ++q;
            (void)quote;
            return tag.substr(st, q - st);
        }
        p = after;
    }
    return "";
}

// Get a presentation property: prefer style="...:..." then the attribute.
std::string prop(const std::string& tag, const std::string& name) {
    std::string style = attr(tag, "style");
    if (!style.empty()) {
        size_t p = style.find(name + ":");
        // ensure it's a property boundary
        while (p != std::string::npos) {
            bool ok = p == 0 || style[p-1] == ';' || style[p-1] == ' ';
            if (ok) { size_t st = p + name.size() + 1; size_t e = style.find(';', st); return style.substr(st, e == std::string::npos ? std::string::npos : e - st); }
            p = style.find(name + ":", p + 1);
        }
    }
    return attr(tag, name);
}

float num(const std::string& s, float def = 0) {
    if (s.empty()) return def;
    return static_cast<float>(std::atof(s.c_str()));
}

// Tokenize a path `d` string into commands/numbers and flatten to subpaths.
void flatten_path(const std::string& d, std::vector<std::vector<Pt>>& out) {
    std::vector<Pt> cur; Pt pos{0,0}, start{0,0}, ctrl{0,0}; char prev = 0;
    size_t i = 0; auto skip = [&]{ while (i < d.size() && (d[i]==' '||d[i]==','||d[i]=='\t'||d[i]=='\n'||d[i]=='\r')) ++i; };
    auto rd = [&]() -> float { skip(); size_t st = i; if (i<d.size()&&(d[i]=='+'||d[i]=='-'))++i; while (i<d.size()&&((d[i]>='0'&&d[i]<='9')||d[i]=='.'||d[i]=='e'||d[i]=='E'||d[i]=='+'||d[i]=='-')){ if((d[i]=='+'||d[i]=='-')&&i>st&&d[i-1]!='e'&&d[i-1]!='E')break; ++i; } return std::atof(d.substr(st, i-st).c_str()); };
    auto bez3 = [&](Pt p0, Pt p1, Pt p2, Pt p3) { for (int k = 1; k <= 12; ++k) { float t = k/12.0f, u = 1-t; cur.push_back({u*u*u*p0.x+3*u*u*t*p1.x+3*u*t*t*p2.x+t*t*t*p3.x, u*u*u*p0.y+3*u*u*t*p1.y+3*u*t*t*p2.y+t*t*t*p3.y}); } };
    auto bez2 = [&](Pt p0, Pt p1, Pt p2) { for (int k = 1; k <= 10; ++k) { float t = k/10.0f, u = 1-t; cur.push_back({u*u*p0.x+2*u*t*p1.x+t*t*p2.x, u*u*p0.y+2*u*t*p1.y+t*t*p2.y}); } };
    while (i < d.size()) {
        skip(); if (i >= d.size()) break;
        char c = d[i];
        if (std::isalpha((unsigned char)c)) { ++i; prev = c; } else { c = prev; if (c=='M') c='L'; if (c=='m') c='l'; }
        bool rel = c >= 'a';
        char C = std::toupper((unsigned char)c);
        if (C == 'M') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } float x=rd(),y=rd(); if(rel){x+=pos.x;y+=pos.y;} pos={x,y}; start=pos; cur.push_back(pos); }
        else if (C == 'L') { float x=rd(),y=rd(); if(rel){x+=pos.x;y+=pos.y;} pos={x,y}; cur.push_back(pos); }
        else if (C == 'H') { float x=rd(); if(rel)x+=pos.x; pos.x=x; cur.push_back(pos); }
        else if (C == 'V') { float y=rd(); if(rel)y+=pos.y; pos.y=y; cur.push_back(pos); }
        else if (C == 'C') { float x1=rd(),y1=rd(),x2=rd(),y2=rd(),x=rd(),y=rd(); if(rel){x1+=pos.x;y1+=pos.y;x2+=pos.x;y2+=pos.y;x+=pos.x;y+=pos.y;} bez3(pos,{x1,y1},{x2,y2},{x,y}); ctrl={x2,y2}; pos={x,y}; }
        else if (C == 'S') { float x2=rd(),y2=rd(),x=rd(),y=rd(); if(rel){x2+=pos.x;y2+=pos.y;x+=pos.x;y+=pos.y;} Pt c1={2*pos.x-ctrl.x,2*pos.y-ctrl.y}; bez3(pos,c1,{x2,y2},{x,y}); ctrl={x2,y2}; pos={x,y}; }
        else if (C == 'Q') { float x1=rd(),y1=rd(),x=rd(),y=rd(); if(rel){x1+=pos.x;y1+=pos.y;x+=pos.x;y+=pos.y;} bez2(pos,{x1,y1},{x,y}); ctrl={x1,y1}; pos={x,y}; }
        else if (C == 'T') { float x=rd(),y=rd(); if(rel){x+=pos.x;y+=pos.y;} Pt c1={2*pos.x-ctrl.x,2*pos.y-ctrl.y}; bez2(pos,c1,{x,y}); ctrl=c1; pos={x,y}; }
        else if (C == 'A') { rd();rd();rd();rd();rd(); float x=rd(),y=rd(); if(rel){x+=pos.x;y+=pos.y;} pos={x,y}; cur.push_back(pos); }  // arc → line (approx)
        else if (C == 'Z') { if(!cur.empty()) cur.push_back(start); pos=start; }
        else { ++i; }
    }
    if (!cur.empty()) out.push_back(cur);
}

// Fill polygons (even-odd) of `subpaths` (already in device space) into buf.
void fill_polys(std::vector<uint8_t>& buf, int W, int H, const std::vector<std::vector<Pt>>& subpaths, Col col) {
    if (col.a <= 0) return;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (auto& sp : subpaths) for (auto& p : sp) { minx=std::min(minx,p.x); miny=std::min(miny,p.y); maxx=std::max(maxx,p.x); maxy=std::max(maxy,p.y); }
    int y0 = std::max(0, (int)std::floor(miny)), y1 = std::min(H, (int)std::ceil(maxy));
    int x0 = std::max(0, (int)std::floor(minx)), x1 = std::min(W, (int)std::ceil(maxx));
    const int SS = 2;  // 2x2 supersample for light AA
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            int hits = 0;
            for (int sy = 0; sy < SS; ++sy) for (int sx = 0; sx < SS; ++sx) {
                float px = x + (sx + 0.5f) / SS, py = y + (sy + 0.5f) / SS;
                bool in = false;
                for (auto& sp : subpaths) {
                    size_t n = sp.size();
                    for (size_t a = 0, b = n - 1; a < n; b = a++) {
                        if (((sp[a].y > py) != (sp[b].y > py)) &&
                            (px < (sp[b].x - sp[a].x) * (py - sp[a].y) / (sp[b].y - sp[a].y) + sp[a].x))
                            in = !in;
                    }
                }
                if (in) ++hits;
            }
            if (!hits) continue;
            float cov = col.a * hits / (SS * SS);
            size_t idx = ((size_t)y * W + x) * 4;
            float inv = 1 - cov;
            buf[idx]   = (uint8_t)(col.r * 255 * cov + buf[idx]   * inv);
            buf[idx+1] = (uint8_t)(col.g * 255 * cov + buf[idx+1] * inv);
            buf[idx+2] = (uint8_t)(col.b * 255 * cov + buf[idx+2] * inv);
            buf[idx+3] = (uint8_t)std::min(255.0f, 255 * cov + buf[idx+3] * inv);
        }
    }
}

Mat parse_transform(const std::string& t) {
    Mat m;
    size_t p = 0;
    while ((p = t.find('(', p)) != std::string::npos) {
        size_t fn = t.find_last_not_of(" ", p - 1);
        size_t fs = t.find_last_of(" ,)", fn) ; fs = (fs == std::string::npos) ? 0 : fs + 1;
        std::string name = t.substr(fs, fn - fs + 1);
        size_t cp = t.find(')', p);
        std::string args = t.substr(p + 1, cp - p - 1);
        float v[6] = {0,0,0,0,0,0}; int n = 0; size_t q = 0;
        while (q < args.size() && n < 6) { while (q<args.size()&&(args[q]==' '||args[q]==',')) ++q; if(q>=args.size())break; v[n++]=std::atof(args.c_str()+q); while(q<args.size()&&args[q]!=' '&&args[q]!=',')++q; }
        Mat cur;
        if (name == "translate") cur = {1,0,0,1,v[0], n>1?v[1]:0};
        else if (name == "scale") cur = {v[0],0,0,n>1?v[1]:v[0],0,0};
        else if (name == "matrix" && n>=6) cur = {v[0],v[1],v[2],v[3],v[4],v[5]};
        m = mul(m, cur);
        p = cp + 1;
    }
    return m;
}

}  // namespace

DecodedImage decode_svg(const uint8_t* data, size_t len, int out_w, int out_h) {
    DecodedImage out;
    if (!data || len < 4 || out_w <= 0 || out_h <= 0) return out;
    std::string s(reinterpret_cast<const char*>(data), len);
    size_t svg = s.find("<svg");
    if (svg == std::string::npos) return out;
    size_t svg_end = s.find('>', svg);
    std::string svg_tag = s.substr(svg, svg_end - svg);

    // Establish the user→device transform from viewBox (or width/height).
    float vbx = 0, vby = 0, vbw = 0, vbh = 0;
    std::string vb = attr(svg_tag, "viewBox");
    if (!vb.empty()) { sscanf(vb.c_str(), "%g %g %g %g", &vbx, &vby, &vbw, &vbh); }
    if (vbw <= 0) vbw = num(attr(svg_tag, "width"), out_w);
    if (vbh <= 0) vbh = num(attr(svg_tag, "height"), out_h);
    if (vbw <= 0 || vbh <= 0) { vbw = out_w; vbh = out_h; }
    float sx = out_w / vbw, sy = out_h / vbh;
    Mat root{sx, 0, 0, sy, -vbx * sx, -vby * sy};

    out.width = out_w; out.height = out_h;
    out.rgba.assign((size_t)out_w * out_h * 4, 0);   // transparent
    out.ok = true;

    // Walk elements sequentially, tracking a simple <g transform> stack.
    std::vector<Mat> stack{root};
    size_t i = svg_end + 1;
    while (i < s.size()) {
        size_t lt = s.find('<', i); if (lt == std::string::npos) break;
        size_t gt = s.find('>', lt); if (gt == std::string::npos) break;
        std::string tag = s.substr(lt, gt - lt + 1);
        i = gt + 1;
        if (tag.rfind("</g", 0) == 0) { if (stack.size() > 1) stack.pop_back(); continue; }
        if (tag.size() < 2 || tag[1] == '/' || tag[1] == '!' || tag[1] == '?') continue;
        std::string name; for (size_t k = 1; k < tag.size() && (std::isalnum((unsigned char)tag[k])); ++k) name += (char)std::tolower(tag[k]);
        Mat tm = stack.back();
        std::string tr = attr(tag, "transform");
        if (!tr.empty()) tm = mul(tm, parse_transform(tr));
        if (name == "g") { stack.push_back(tm); continue; }

        std::vector<std::vector<Pt>> polys;
        if (name == "rect") {
            float x = num(attr(tag,"x")), y = num(attr(tag,"y")), w = num(attr(tag,"width")), h = num(attr(tag,"height"));
            polys.push_back({{x,y},{x+w,y},{x+w,y+h},{x,y+h}});
        } else if (name == "circle") {
            float cx = num(attr(tag,"cx")), cy = num(attr(tag,"cy")), r = num(attr(tag,"r")); std::vector<Pt> c;
            for (int k = 0; k < 48; ++k) { float a = k * 6.2831853f / 48; c.push_back({cx + r*std::cos(a), cy + r*std::sin(a)}); } polys.push_back(c);
        } else if (name == "ellipse") {
            float cx = num(attr(tag,"cx")), cy = num(attr(tag,"cy")), rx = num(attr(tag,"rx")), ry = num(attr(tag,"ry")); std::vector<Pt> c;
            for (int k = 0; k < 48; ++k) { float a = k * 6.2831853f / 48; c.push_back({cx + rx*std::cos(a), cy + ry*std::sin(a)}); } polys.push_back(c);
        } else if (name == "polygon" || name == "polyline") {
            std::string pts = attr(tag, "points"); std::vector<Pt> c; const char* p = pts.c_str(); char* end;
            while (*p) { float x = std::strtof(p, &end); if (end==p) break; p=end; while(*p==' '||*p==',')++p; float y = std::strtof(p,&end); p=end; while(*p==' '||*p==',')++p; c.push_back({x,y}); }
            if (!c.empty()) polys.push_back(c);
        } else if (name == "line") {
            polys.push_back({{num(attr(tag,"x1")),num(attr(tag,"y1"))},{num(attr(tag,"x2")),num(attr(tag,"y2"))}});
        } else if (name == "path") {
            flatten_path(attr(tag, "d"), polys);
        } else continue;

        // device-transform the points
        for (auto& sp : polys) for (auto& p : sp) p = apply(tm, p);

        std::string fillv = prop(tag, "fill");
        Col fill = fillv.empty() ? Col{0,0,0,1} : parse_color(fillv);   // default fill is black
        std::string fo = prop(tag, "fill-opacity"); if (!fo.empty()) fill.a *= num(fo, 1);
        std::string op = prop(tag, "opacity"); if (!op.empty()) fill.a *= num(op, 1);
        fill_polys(out.rgba, out_w, out_h, polys, fill);
    }
    return out;
}

} // namespace malibu::image
