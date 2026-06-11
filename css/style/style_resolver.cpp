// css/style/style_resolver.cpp
// Cascade resolution, inheritance, var() substitution, and value parsing.

#include "malibu/css/style/style_resolver.h"
#include "malibu/dom/document.h"
#include "malibu/diagnostics/diagnostic_log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>

namespace malibu::css {
namespace {

using malibu::dom::Document;
using malibu::dom::NodeCore;

std::string narrow(const std::u16string& s) { std::string r; for (char16_t c : s) r.push_back(static_cast<char>(c & 0xFF)); return r; }
std::u16string lower(std::u16string s) { for (auto& c : s) if (c >= u'A' && c <= u'Z') c = c - u'A' + u'a'; return s; }
bool ws(char16_t c) { return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == u'\f'; }
std::u16string trim(std::u16string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && ws(s[b])) ++b;
    while (e > b && ws(s[e - 1])) --e;
    return std::u16string(s.substr(b, e - b));
}

// Evaluates an @media condition against the viewport (px). Commas = OR; within
// one query, `(feature: value)` groups are ANDed. Supports min/max-width,
// width, min/max-height, orientation, prefers-color-scheme(light), and the
// print/screen media types. Unknown features are treated as matching.
bool media_matches(const std::u16string& cond, float vw, float vh) {
    std::u16string c = lower(cond);
    size_t start = 0;
    while (start <= c.size()) {
        size_t comma = c.find(u',', start);
        std::u16string q = c.substr(start, comma == std::u16string::npos ? std::u16string::npos : comma - start);
        start = comma == std::u16string::npos ? c.size() + 1 : comma + 1;
        bool ok = true;
        if (q.find(u"print") != std::u16string::npos) ok = false;   // we are a screen
        size_t p = 0;
        while (ok) {
            size_t op = q.find(u'(', p); if (op == std::u16string::npos) break;
            size_t cp = q.find(u')', op); if (cp == std::u16string::npos) break;
            std::u16string feat = q.substr(op + 1, cp - op - 1); p = cp + 1;
            size_t colon = feat.find(u':');
            std::u16string name = trim(colon == std::u16string::npos ? feat : feat.substr(0, colon));
            std::u16string val = colon == std::u16string::npos ? std::u16string() : trim(feat.substr(colon + 1));
            std::string num; for (char16_t ch : val) if ((ch >= u'0' && ch <= u'9') || ch == u'.') num.push_back((char)ch);
            float n = num.empty() ? 0.0f : static_cast<float>(std::atof(num.c_str()));
            if (val.find(u"rem") != std::u16string::npos || val.find(u"em") != std::u16string::npos) n *= 16.0f;
            if (name == u"min-width")       { if (vw < n)  ok = false; }
            else if (name == u"max-width")  { if (vw > n)  ok = false; }
            else if (name == u"width")      { if (vw != n) ok = false; }
            else if (name == u"min-height") { if (vh < n)  ok = false; }
            else if (name == u"max-height") { if (vh > n)  ok = false; }
            else if (name == u"orientation") {
                bool land = vw >= vh;
                if (val.find(u"portrait") != std::u16string::npos && land) ok = false;
                if (val.find(u"landscape") != std::u16string::npos && !land) ok = false;
            } else if (name == u"prefers-color-scheme") {
                if (val.find(u"dark") != std::u16string::npos) ok = false;  // we render light
            }
        }
        if (ok) return true;
    }
    return false;
}

// Split into top-level whitespace-separated tokens (parentheses are atomic).
std::vector<std::u16string> split_tokens(const std::u16string& s) {
    std::vector<std::u16string> out;
    std::u16string cur;
    int depth = 0;
    for (char16_t c : s) {
        if (c == u'(') ++depth;
        else if (c == u')') --depth;
        if (ws(c) && depth == 0) { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Split function args by top-level commas.
std::vector<std::u16string> split_commas(const std::u16string& s) {
    std::vector<std::u16string> out;
    std::u16string cur;
    int depth = 0;
    for (char16_t c : s) {
        if (c == u'(') ++depth;
        else if (c == u')') --depth;
        if (c == u',' && depth == 0) { out.push_back(trim(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(trim(cur));
    return out;
}

float parse_float(const std::u16string& s) {
    try { return std::stof(narrow(s)); } catch (...) { return 0.0f; }
}

// calc() evaluator. calc is linear, so we accumulate per-unit coefficients.
struct Coeffs { float px = 0, pct = 0, em = 0, rem = 0, vw = 0, vh = 0, num = 0; bool is_num = true; };
struct CalcParser {
    const std::u16string& s; size_t i = 0;
    void ws() { while (i < s.size() && (s[i] == u' ' || s[i] == u'\t')) ++i; }
    void add(Coeffs& a, const Coeffs& b, float sg) {
        a.px += sg*b.px; a.pct += sg*b.pct; a.em += sg*b.em; a.rem += sg*b.rem; a.vw += sg*b.vw; a.vh += sg*b.vh; a.num += sg*b.num;
        a.is_num = a.is_num && b.is_num;
    }
    void scale(Coeffs& a, float k) { a.px*=k; a.pct*=k; a.em*=k; a.rem*=k; a.vw*=k; a.vh*=k; a.num*=k; }
    Coeffs factor() {
        ws();
        if (i < s.size() && s[i] == u'(') { ++i; Coeffs c = expr(); ws(); if (i < s.size() && s[i] == u')') ++i; return c; }
        size_t st = i; if (i < s.size() && (s[i] == u'+' || s[i] == u'-')) ++i;
        while (i < s.size() && ((s[i] >= u'0' && s[i] <= u'9') || s[i] == u'.')) ++i;
        float v = parse_float(s.substr(st, i - st));
        std::u16string u; while (i < s.size() && ((s[i] >= u'a' && s[i] <= u'z') || s[i] == u'%')) u.push_back(s[i++]);
        Coeffs c; c.is_num = false;
        if (u == u"%") c.pct = v; else if (u == u"px") c.px = v; else if (u == u"em") c.em = v;
        else if (u == u"rem") c.rem = v; else if (u == u"vw") c.vw = v; else if (u == u"vh") c.vh = v;
        else { c.num = v; c.is_num = true; }
        return c;
    }
    Coeffs term() {
        Coeffs a = factor();
        for (;;) { ws(); if (i < s.size() && (s[i] == u'*' || s[i] == u'/')) { char16_t op = s[i++]; Coeffs b = factor();
            if (op == u'*') { if (b.is_num) scale(a, b.num); else if (a.is_num) { float k = a.num; a = b; scale(a, k); } }
            else { if (b.is_num && b.num != 0) scale(a, 1.0f / b.num); } } else break; }
        return a;
    }
    Coeffs expr() {
        Coeffs a = term();
        for (;;) { ws(); if (i < s.size() && (s[i] == u'+' || s[i] == u'-')) { char16_t op = s[i++]; Coeffs b = term(); add(a, b, op == u'+' ? 1.0f : -1.0f); } else break; }
        return a;
    }
};
Length parse_calc(const std::u16string& inner) {
    CalcParser p{inner};
    Coeffs c = p.expr();
    Length L; L.unit = LengthUnit::Calc;
    L.c_px = c.px + c.num;  // bare number in a length context → px
    L.c_pct = c.pct; L.c_em = c.em; L.c_rem = c.rem; L.c_vw = c.vw; L.c_vh = c.vh;
    return L;
}

Length parse_length(const std::u16string& in) {
    std::u16string s = lower(trim(in));
    if (s == u"auto") return Length::auto_();
    if (s == u"0") return Length::px(0);
    if (s.rfind(u"calc(", 0) == 0) {
        size_t rp = s.rfind(u')');
        return parse_calc(s.substr(5, (rp == std::u16string::npos ? s.size() : rp) - 5));
    }
    auto ends = [&](const char16_t* suf) {
        std::u16string u(suf);
        return s.size() > u.size() && s.compare(s.size() - u.size(), u.size(), u) == 0;
    };
    if (ends(u"px"))  return {parse_float(s.substr(0, s.size() - 2)), LengthUnit::Px};
    if (ends(u"rem")) return {parse_float(s.substr(0, s.size() - 3)), LengthUnit::Rem};
    if (ends(u"em"))  return {parse_float(s.substr(0, s.size() - 2)), LengthUnit::Em};
    if (ends(u"vw"))  return {parse_float(s.substr(0, s.size() - 2)), LengthUnit::Vw};
    if (ends(u"vh"))  return {parse_float(s.substr(0, s.size() - 2)), LengthUnit::Vh};
    if (!s.empty() && s.back() == u'%') return Length::percent(parse_float(s.substr(0, s.size() - 1)));
    return Length::px(parse_float(s));  // unitless → px
}

const std::unordered_map<std::u16string, Color>& named_colors() {
    static const std::unordered_map<std::u16string, Color> m = {
        {u"transparent", {0, 0, 0, 0}}, {u"black", {0, 0, 0, 255}}, {u"white", {255, 255, 255, 255}},
        {u"red", {255, 0, 0, 255}}, {u"green", {0, 128, 0, 255}}, {u"blue", {0, 0, 255, 255}},
        {u"lime", {0, 255, 0, 255}}, {u"yellow", {255, 255, 0, 255}}, {u"cyan", {0, 255, 255, 255}},
        {u"magenta", {255, 0, 255, 255}}, {u"gray", {128, 128, 128, 255}}, {u"grey", {128, 128, 128, 255}},
        {u"silver", {192, 192, 192, 255}}, {u"maroon", {128, 0, 0, 255}}, {u"olive", {128, 128, 0, 255}},
        {u"navy", {0, 0, 128, 255}}, {u"purple", {128, 0, 128, 255}}, {u"teal", {0, 128, 128, 255}},
        {u"orange", {255, 165, 0, 255}}, {u"pink", {255, 192, 203, 255}},
    };
    return m;
}

uint8_t hex_pair(char16_t a, char16_t b) {
    auto h = [](char16_t c) -> int {
        if (c >= u'0' && c <= u'9') return c - u'0';
        if (c >= u'a' && c <= u'f') return c - u'a' + 10;
        if (c >= u'A' && c <= u'F') return c - u'A' + 10;
        return 0;
    };
    return static_cast<uint8_t>(h(a) * 16 + h(b));
}

std::optional<Color> parse_color(const std::u16string& in, const Color& current_color) {
    std::u16string s = trim(in);
    std::u16string ls = lower(s);
    if (ls == u"currentcolor") return current_color;
    auto named = named_colors().find(ls);
    if (named != named_colors().end()) return named->second;
    if (!s.empty() && s[0] == u'#') {
        std::u16string h = s.substr(1);
        if (h.size() == 3) return Color{hex_pair(h[0], h[0]), hex_pair(h[1], h[1]), hex_pair(h[2], h[2]), 255};
        if (h.size() == 6) return Color{hex_pair(h[0], h[1]), hex_pair(h[2], h[3]), hex_pair(h[4], h[5]), 255};
        if (h.size() == 8) return Color{hex_pair(h[0], h[1]), hex_pair(h[2], h[3]), hex_pair(h[4], h[5]), hex_pair(h[6], h[7])};
        return std::nullopt;
    }
    if (ls.rfind(u"rgb", 0) == 0) {
        size_t lp = s.find(u'('), rp = s.find(u')');
        if (lp != std::u16string::npos && rp != std::u16string::npos) {
            auto parts = split_commas(s.substr(lp + 1, rp - lp - 1));
            if (parts.size() >= 3) {
                Color c;
                c.r = static_cast<uint8_t>(std::clamp(parse_float(parts[0]), 0.0f, 255.0f));
                c.g = static_cast<uint8_t>(std::clamp(parse_float(parts[1]), 0.0f, 255.0f));
                c.b = static_cast<uint8_t>(std::clamp(parse_float(parts[2]), 0.0f, 255.0f));
                c.a = parts.size() >= 4 ? static_cast<uint8_t>(std::clamp(parse_float(parts[3]) * 255.0f, 0.0f, 255.0f)) : 255;
                return c;
            }
        }
    }
    return std::nullopt;
}

std::optional<Color> parse_svg_paint(const std::u16string& value,
                                     const Color& current_color,
                                     bool& uses_current_color) {
    uses_current_color = false;
    const std::u16string lowered = lower(trim(value));
    if (lowered == u"none") return Color{0, 0, 0, 0};
    if (lowered == u"currentcolor") {
        uses_current_color = true;
        return current_color;
    }
    if (lowered.rfind(u"url(", 0) == 0) {
        // Paint servers are not rasterized yet. CSS/SVG permits a fallback
        // paint after url(...), which still gives deterministic useful output.
        auto tokens = split_tokens(value);
        for (auto it = tokens.rbegin(); it != tokens.rend(); ++it) {
            if (lower(*it) == u"currentcolor") {
                uses_current_color = true;
                return current_color;
            }
            if (auto color = parse_color(*it, current_color)) return color;
        }
        return std::nullopt;
    }
    return parse_color(value, current_color);
}

void parse_box_shorthand(const std::u16string& value, BoxEdge& edge) {
    auto toks = split_tokens(value);
    std::vector<Length> v;
    for (auto& t : toks) v.push_back(parse_length(t));
    if (v.empty()) return;
    if (v.size() == 1) edge = {v[0], v[0], v[0], v[0]};
    else if (v.size() == 2) edge = {v[0], v[1], v[0], v[1]};
    else if (v.size() == 3) edge = {v[0], v[1], v[2], v[1]};
    else edge = {v[0], v[1], v[2], v[3]};
}

// box-shadow: [inset] <x> <y> [blur] [spread] [color]. First shadow only.
void parse_box_shadow(const std::u16string& value, ComputedStyle& s) {
    if (lower(trim(value)) == u"none") { s.has_box_shadow = false; return; }
    std::u16string first = split_commas(value).front();
    std::vector<float> nums; Color col{0, 0, 0, 160}; bool got_c = false;
    for (auto& t : split_tokens(first)) {
        if (lower(t) == u"inset") continue;
        if (auto c = parse_color(t, s.color)) { col = *c; got_c = true; continue; }
        nums.push_back(parse_length(t).resolve(s.font_size, 0, 16.0f, 0, 0));
    }
    (void)got_c;
    if (nums.size() >= 2) {
        s.shadow_x = nums[0]; s.shadow_y = nums[1];
        s.shadow_blur = nums.size() > 2 ? nums[2] : 0.0f;
        s.shadow_spread = nums.size() > 3 ? nums[3] : 0.0f;
        s.shadow_color = col; s.has_box_shadow = true;
    }
}

// background-image: linear-gradient([<angle>deg | to <side>,] stop, stop, ...).
bool parse_linear_gradient(const std::u16string& value, ComputedStyle& s) {
    std::u16string lv = lower(value);
    size_t lg = lv.find(u"linear-gradient(");
    if (lg == std::u16string::npos) return false;
    size_t open = value.find(u'(', lg), close = value.rfind(u')');
    if (open == std::u16string::npos || close == std::u16string::npos || close <= open) return false;
    auto parts = split_commas(value.substr(open + 1, close - open - 1));
    if (parts.empty()) return false;
    std::vector<ComputedStyle::GradientStop> stops;
    float angle = 180.0f;
    size_t i0 = 0;
    std::u16string p0 = lower(trim(parts[0]));
    if (p0.find(u"deg") != std::u16string::npos) {
        angle = parse_float(p0); i0 = 1;
    } else if (p0.rfind(u"to ", 0) == 0) {
        bool top = p0.find(u"top") != std::u16string::npos;
        bool bot = p0.find(u"bottom") != std::u16string::npos;
        bool left = p0.find(u"left") != std::u16string::npos;
        bool right = p0.find(u"right") != std::u16string::npos;
        if (right && !top && !bot)      angle = 90;
        else if (left && !top && !bot)  angle = 270;
        else if (top)                   angle = 0;
        else                            angle = 180;
        i0 = 1;
    }
    for (size_t i = i0; i < parts.size(); ++i) {
        Color c{0, 0, 0, 255}; float pos = -1.0f; bool got = false;
        for (auto& t : split_tokens(trim(parts[i]))) {
            if (!t.empty() && t.back() == u'%') { pos = parse_float(t) / 100.0f; }
            else if (auto cc = parse_color(t, s.color)) { c = *cc; got = true; }
        }
        if (got) stops.push_back({c, pos});
    }
    if (stops.size() < 2) return false;
    for (size_t i = 0; i < stops.size(); ++i)             // fill missing positions evenly
        if (stops[i].pos < 0) stops[i].pos = stops.size() == 1 ? 0.0f : (float)i / (stops.size() - 1);
    s.bg_gradient = true; s.bg_angle = angle; s.bg_stops = std::move(stops);
    return true;
}

Transform2D mul(const Transform2D& m, const Transform2D& n) {
    return {m.a * n.a + m.c * n.b,       m.b * n.a + m.d * n.b,
            m.a * n.c + m.c * n.d,       m.b * n.c + m.d * n.d,
            m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f};
}

Transform2D parse_transform(const std::u16string& value, float font_size) {
    Transform2D result;  // identity
    for (auto& fn : split_tokens(value)) {
        size_t lp = fn.find(u'(');
        if (lp == std::u16string::npos) continue;
        std::u16string name = lower(fn.substr(0, lp));
        std::u16string argstr = fn.substr(lp + 1, fn.find(u')', lp) - lp - 1);
        auto args = split_commas(argstr);
        Transform2D t;
        if (name == u"translate" || name == u"translatex" || name == u"translatey") {
            float x = 0, y = 0;
            if (name == u"translatey") y = parse_length(args[0]).resolve(font_size, 0);
            else {
                if (!args.empty()) x = parse_length(args[0]).resolve(font_size, 0);
                if (name == u"translate" && args.size() > 1) y = parse_length(args[1]).resolve(font_size, 0);
            }
            t = {1, 0, 0, 1, x, y};
        } else if (name == u"scale" || name == u"scalex" || name == u"scaley") {
            float sx = 1, sy = 1;
            if (name == u"scalex") sx = parse_float(args[0]);
            else if (name == u"scaley") sy = parse_float(args[0]);
            else { sx = args.empty() ? 1 : parse_float(args[0]); sy = args.size() > 1 ? parse_float(args[1]) : sx; }
            t = {sx, 0, 0, sy, 0, 0};
        } else if (name == u"rotate") {
            float deg = parse_float(args.empty() ? u"0" : args[0]);
            float rad = deg * 3.14159265358979323846f / 180.0f;
            t = {std::cos(rad), std::sin(rad), -std::sin(rad), std::cos(rad), 0, 0};
        } else if (name == u"matrix" && args.size() == 6) {
            t = {parse_float(args[0]), parse_float(args[1]), parse_float(args[2]),
                 parse_float(args[3]), parse_float(args[4]), parse_float(args[5])};
        } else continue;
        result = mul(result, t);
    }
    return result;
}

// Replace var(--name[, fallback]) using the element's resolved custom props.
std::u16string substitute_vars(const std::u16string& value,
                               const std::unordered_map<std::u16string, std::u16string>& props) {
    if (value.find(u"var(") == std::u16string::npos) return value;
    std::u16string out;
    size_t i = 0;
    while (i < value.size()) {
        if (value.compare(i, 4, u"var(") == 0) {
            int depth = 1; size_t j = i + 4;
            std::u16string inner;
            while (j < value.size() && depth > 0) {
                if (value[j] == u'(') ++depth;
                else if (value[j] == u')') { if (--depth == 0) break; }
                inner.push_back(value[j++]);
            }
            i = (j < value.size()) ? j + 1 : value.size();
            auto args = split_commas(inner);
            std::u16string name = trim(args.empty() ? u"" : args[0]);
            auto it = props.find(name);
            if (it != props.end()) out += substitute_vars(it->second, props);
            else if (args.size() > 1) out += substitute_vars(trim(args[1]), props);
        } else {
            out.push_back(value[i++]);
        }
    }
    return out;
}

// CSS <absolute-size> / <relative-size> keywords → px (medium = 16px, matching
// the legacy HTML font-size table browsers use). Returns -1 for a non-keyword.
float font_size_keyword(const std::u16string& kw, float parent_size) {
    if (kw == u"xx-small") return 9.0f;
    if (kw == u"x-small")  return 10.0f;
    if (kw == u"small")    return 13.0f;
    if (kw == u"medium")   return 16.0f;
    if (kw == u"large")    return 18.0f;
    if (kw == u"x-large")  return 24.0f;
    if (kw == u"xx-large") return 32.0f;
    if (kw == u"smaller")  return parent_size / 1.2f;
    if (kw == u"larger")   return parent_size * 1.2f;
    return -1.0f;
}

void apply_property(ComputedStyle& s, const ComputedStyle& parent,
                    const std::u16string& prop, const std::u16string& raw_value) {
    std::u16string value = trim(raw_value);
    std::u16string lv = lower(value);

    if (lv == u"inherit") {
        // Copy the parent's value for this property where meaningful.
        if (prop == u"color") s.color = parent.color;
        else if (prop == u"fill") {
            s.svg_fill = parent.svg_fill;
            s.svg_fill_current_color = parent.svg_fill_current_color;
            s.svg_fill_specified = parent.svg_fill_specified;
        }
        else if (prop == u"font-size") s.font_size = parent.font_size;
        else if (prop == u"font-family") s.font_family = parent.font_family;
        else if (prop == u"visibility") s.visibility = parent.visibility;
        return;
    }
    if (lv == u"initial" || lv == u"unset") return;  // keep initialised value

    if (prop == u"display") {
        if (lv == u"block") s.display = DisplayType::Block;
        else if (lv == u"inline") s.display = DisplayType::Inline;
        else if (lv == u"inline-block") s.display = DisplayType::InlineBlock;
        else if (lv == u"flex") s.display = DisplayType::Flex;
        else if (lv == u"inline-flex") s.display = DisplayType::InlineFlex;
        else if (lv == u"list-item") s.display = DisplayType::ListItem;
        else if (lv == u"table" || lv == u"inline-table") s.display = DisplayType::Table;
        else if (lv == u"table-row") s.display = DisplayType::TableRow;
        else if (lv == u"table-cell") s.display = DisplayType::TableCell;
        else if (lv == u"table-row-group" || lv == u"table-header-group" || lv == u"table-footer-group") s.display = DisplayType::TableRowGroup;
        else if (lv == u"grid") s.display = DisplayType::Grid;
        else if (lv == u"inline-grid") s.display = DisplayType::Grid;
        else if (lv == u"none") s.display = DisplayType::None;
        else if (lv == u"inline-flex") s.display = DisplayType::InlineFlex;
        else s.display = DisplayType::Block;  // grid/table-* others → block fallback
    } else if (prop == u"float") {
        s.float_ = (lv == u"left") ? FloatType::Left : (lv == u"right") ? FloatType::Right : FloatType::None;
    } else if (prop == u"clear") {
        s.clear = (lv == u"left") ? ClearType::Left : (lv == u"right") ? ClearType::Right
                : (lv == u"both") ? ClearType::Both : ClearType::None;
    } else if (prop == u"text-decoration" || prop == u"text-decoration-line") {
        if (lv.find(u"underline") != std::u16string::npos) s.text_decoration = TextDecoration::Underline;
        else if (lv.find(u"line-through") != std::u16string::npos) s.text_decoration = TextDecoration::LineThrough;
        else if (lv.find(u"overline") != std::u16string::npos) s.text_decoration = TextDecoration::Overline;
        else s.text_decoration = TextDecoration::None;
    } else if (prop == u"font-style") {
        s.font_style = (lv == u"italic") ? FontStyle::Italic : (lv == u"oblique") ? FontStyle::Oblique : FontStyle::Normal;
    } else if (prop == u"list-style-type" || prop == u"list-style") {
        if (lv.find(u"none") != std::u16string::npos) s.list_style = ListStyleType::None;
        else if (lv.find(u"decimal") != std::u16string::npos) s.list_style = ListStyleType::Decimal;
        else if (lv.find(u"circle") != std::u16string::npos) s.list_style = ListStyleType::Circle;
        else if (lv.find(u"square") != std::u16string::npos) s.list_style = ListStyleType::Square;
        else s.list_style = ListStyleType::Disc;
    } else if (prop == u"white-space") {
        s.white_space = (lv == u"nowrap") ? WhiteSpace::NoWrap : (lv == u"pre") ? WhiteSpace::Pre
                      : (lv == u"pre-wrap") ? WhiteSpace::PreWrap : WhiteSpace::Normal;
    } else if (prop == u"text-transform") {
        s.text_transform = (lv == u"uppercase") ? TextTransform::Uppercase : (lv == u"lowercase") ? TextTransform::Lowercase
                         : (lv == u"capitalize") ? TextTransform::Capitalize : TextTransform::None;
    } else if (prop == u"vertical-align") {
        s.vertical_align = (lv == u"top") ? VerticalAlign::Top : (lv == u"middle") ? VerticalAlign::Middle
                         : (lv == u"bottom") ? VerticalAlign::Bottom : (lv == u"sub") ? VerticalAlign::Sub
                         : (lv == u"super") ? VerticalAlign::Super : VerticalAlign::Baseline;
    } else if (prop == u"border-radius") {
        s.border_radius = parse_length(value).resolve(s.font_size, 0, 16.0f, 0, 0);
    } else if (prop == u"aspect-ratio") {
        // "W / H" or a single number.
        size_t slash = value.find(u'/');
        if (slash != std::u16string::npos) {
            float w = parse_float(trim(value.substr(0, slash))), hh = parse_float(trim(value.substr(slash + 1)));
            s.aspect_ratio = (hh > 0) ? w / hh : 0.0f;
        } else { float r = parse_float(value); s.aspect_ratio = r > 0 ? r : 0.0f; }
    } else if (prop == u"grid-template-columns") {
        s.grid_template_columns = value;
    } else if (prop == u"gap" || prop == u"grid-gap" || prop == u"column-gap" || prop == u"row-gap") {
        // `gap` shorthand may carry "row col"; we model a single gap → take the first.
        std::u16string g = value; auto sp = g.find(u' '); if (sp != std::u16string::npos) g = g.substr(0, sp);
        s.gap = parse_length(g).resolve(s.font_size, 0, 16.0f, 0, 0);
    } else if (prop == u"position") {
        if (lv == u"relative") s.position = PositionType::Relative;
        else if (lv == u"absolute") s.position = PositionType::Absolute;
        else if (lv == u"fixed") s.position = PositionType::Fixed;
        else if (lv == u"sticky") s.position = PositionType::Sticky;
        else s.position = PositionType::Static;
    } else if (prop == u"box-sizing") {
        s.box_sizing = (lv == u"border-box") ? BoxSizing::BorderBox : BoxSizing::ContentBox;
    } else if (prop == u"width")  s.width = parse_length(value);
    else if (prop == u"height") s.height = parse_length(value);
    else if (prop == u"max-width")  s.max_width = parse_length(value);
    else if (prop == u"min-width")  s.min_width = parse_length(value);
    else if (prop == u"max-height") s.max_height = parse_length(value);
    else if (prop == u"min-height") s.min_height = parse_length(value);
    else if (prop == u"top")    s.top = parse_length(value);
    else if (prop == u"right")  s.right = parse_length(value);
    else if (prop == u"bottom") s.bottom = parse_length(value);
    else if (prop == u"left")   s.left = parse_length(value);
    else if (prop == u"margin") parse_box_shorthand(value, s.margin);
    else if (prop == u"padding") parse_box_shorthand(value, s.padding);
    else if (prop == u"margin-top") s.margin.top = parse_length(value);
    else if (prop == u"margin-right") s.margin.right = parse_length(value);
    else if (prop == u"margin-bottom") s.margin.bottom = parse_length(value);
    else if (prop == u"margin-left") s.margin.left = parse_length(value);
    else if (prop == u"padding-top") s.padding.top = parse_length(value);
    else if (prop == u"padding-right") s.padding.right = parse_length(value);
    else if (prop == u"padding-bottom") s.padding.bottom = parse_length(value);
    else if (prop == u"padding-left") s.padding.left = parse_length(value);
    else if (prop == u"border-width") parse_box_shorthand(value, s.border);
    else if (prop == u"border" || prop == u"border-top" || prop == u"border-right" ||
             prop == u"border-bottom" || prop == u"border-left") {
        // Parse a border shorthand ([width] [style] [color]); apply to all sides
        // or one. `none`/`hidden`/style:none ⇒ width 0.
        bool none = false, got_w = false, got_style = false; Length w = Length::px(0); Color col = s.color; bool got_c = false;
        for (auto& t : split_tokens(value)) {
            std::u16string lt = lower(t);
            if (lt == u"none" || lt == u"hidden") { none = true; continue; }
            if (lt == u"solid" || lt == u"dashed" || lt == u"dotted" || lt == u"double" ||
                lt == u"groove" || lt == u"ridge" || lt == u"inset" || lt == u"outset") { got_style = true; continue; }
            if (auto c = parse_color(t, s.color)) { col = *c; got_c = true; continue; }
            w = parse_length(t); got_w = true;
        }
        if (!none && !got_w && got_style) w = Length::px(3);   // "medium" default
        Length* W[4] = {&s.border.top, &s.border.right, &s.border.bottom, &s.border.left};
        auto apply = [&](int i) { *W[i] = none ? Length::px(0) : w; if (got_c) { s.border_colors[i] = col; s.border_color = col; } };
        if (prop == u"border") { for (int i = 0; i < 4; ++i) apply(i); }
        else if (prop == u"border-top") apply(0);
        else if (prop == u"border-right") apply(1);
        else if (prop == u"border-bottom") apply(2);
        else apply(3);
    }
    else if (prop == u"border-top-width") s.border.top = parse_length(value);
    else if (prop == u"border-right-width") s.border.right = parse_length(value);
    else if (prop == u"border-bottom-width") s.border.bottom = parse_length(value);
    else if (prop == u"border-left-width") s.border.left = parse_length(value);
    else if (prop == u"border-style") { if (lv == u"none" || lv == u"hidden") s.border = {Length::px(0), Length::px(0), Length::px(0), Length::px(0)}; }
    else if (prop == u"border-top-color") { if (auto c = parse_color(value, s.color)) { s.border_colors[0] = *c; s.border_color = *c; } }
    else if (prop == u"border-right-color") { if (auto c = parse_color(value, s.color)) { s.border_colors[1] = *c; s.border_color = *c; } }
    else if (prop == u"border-bottom-color") { if (auto c = parse_color(value, s.color)) { s.border_colors[2] = *c; s.border_color = *c; } }
    else if (prop == u"border-left-color") { if (auto c = parse_color(value, s.color)) { s.border_colors[3] = *c; s.border_color = *c; } }
    else if (prop == u"border-color") { if (auto c = parse_color(value, s.color)) { s.border_color = *c; for (auto& bc : s.border_colors) bc = *c; } }
    else if (prop == u"color") { if (auto c = parse_color(value, parent.color)) s.color = *c; }
    else if (prop == u"fill") {
        bool current_color = false;
        if (auto paint = parse_svg_paint(value, s.color, current_color)) {
            s.svg_fill = *paint;
            s.svg_fill_current_color = current_color;
            s.svg_fill_specified = true;
        }
    }
    else if (prop == u"box-shadow") parse_box_shadow(value, s);
    else if (prop == u"background-image") { parse_linear_gradient(value, s); }
    else if (prop == u"background-color") { if (auto c = parse_color(value, s.color)) s.background_color = *c; }
    else if (prop == u"background") {
        // shorthand: a gradient, and/or a solid color.
        if (!parse_linear_gradient(value, s)) { if (auto c = parse_color(value, s.color)) s.background_color = *c; }
    }
    else if (prop == u"opacity") s.opacity = std::clamp(parse_float(value), 0.0f, 1.0f);
    else if (prop == u"z-index") { if (lv != u"auto") { s.z_index = static_cast<int32_t>(parse_float(value)); s.has_z_index = true; } }
    else if (prop == u"visibility") {
        s.visibility = (lv == u"hidden") ? VisibilityType::Hidden : (lv == u"collapse") ? VisibilityType::Collapse : VisibilityType::Visible;
    } else if (prop == u"overflow") {
        s.overflow = (lv == u"hidden") ? OverflowType::Hidden : (lv == u"scroll") ? OverflowType::Scroll : (lv == u"auto") ? OverflowType::Auto : OverflowType::Visible;
    } else if (prop == u"object-fit") {
        s.object_fit = (lv == u"contain") ? ObjectFit::Contain : (lv == u"cover") ? ObjectFit::Cover
                     : (lv == u"none") ? ObjectFit::None : (lv == u"scale-down") ? ObjectFit::ScaleDown : ObjectFit::Fill;
    } else if (prop == u"font-size") {
        float kw = font_size_keyword(lv, parent.font_size);
        if (kw >= 0.0f) s.font_size = kw;
        else s.font_size = parse_length(value).resolve(parent.font_size, parent.font_size, 16.0f);
    } else if (prop == u"font-family") s.font_family = value;
    else if (prop == u"font") {
        // Shorthand: [style] [variant] [weight] [stretch] size[/line-height] family.
        // System-font keywords (caption/menu/...) are left at inherited values.
        if (lv == u"caption" || lv == u"icon" || lv == u"menu" || lv == u"message-box" ||
            lv == u"small-caption" || lv == u"status-bar") { /* keep inherited */ }
        else {
            std::vector<std::u16string> toks;
            { std::u16string cur;
              for (char16_t c : value) {
                  if (c == u' ' || c == u'\t' || c == u'\n' || c == u'\r') { if (!cur.empty()) { toks.push_back(cur); cur.clear(); } }
                  else cur.push_back(c);
              }
              if (!cur.empty()) toks.push_back(cur); }
            // The size is the first token that is a size keyword or begins a length
            // (digit, dot, sign) — possibly "size/line-height". Tokens before it are
            // style/variant/weight; tokens from there on (joined) are the family.
            size_t si = toks.size();
            for (size_t i = 0; i < toks.size(); ++i) {
                std::u16string sz = lower(toks[i]);
                auto slash = sz.find(u'/'); if (slash != std::u16string::npos) sz = sz.substr(0, slash);
                char16_t c0 = sz.empty() ? 0 : sz[0];
                bool numeric = (c0 >= u'0' && c0 <= u'9') || c0 == u'.' || c0 == u'+' || c0 == u'-';
                if (font_size_keyword(sz, parent.font_size) >= 0.0f || numeric) { si = i; break; }
            }
            if (si < toks.size()) {
                // weight/style keywords before the size
                for (size_t i = 0; i < si; ++i) {
                    std::u16string t = lower(toks[i]);
                    if (t == u"italic" || t == u"oblique") s.font_style = FontStyle::Italic;
                    else if (t == u"bold") s.font_weight = FontWeight::Bold;
                    else if (t.size() == 3 && t[0] >= u'1' && t[0] <= u'9' && t[1] == u'0' && t[2] == u'0')
                        s.font_weight = static_cast<FontWeight>(static_cast<uint16_t>(parse_float(t)));
                }
                std::u16string sztok = toks[si];
                auto slash = sztok.find(u'/');
                std::u16string sizepart = (slash == std::u16string::npos) ? sztok : sztok.substr(0, slash);
                float kw = font_size_keyword(lower(sizepart), parent.font_size);
                s.font_size = (kw >= 0.0f) ? kw : parse_length(sizepart).resolve(parent.font_size, parent.font_size, 16.0f);
                if (slash != std::u16string::npos) {
                    std::u16string lh = sztok.substr(slash + 1);
                    if (!lh.empty() && lh.back() != u'%' && lower(lh).find(u"px") == std::u16string::npos &&
                        ((lh[0] >= u'0' && lh[0] <= u'9') || lh[0] == u'.')) s.line_height = parse_float(lh);
                    else s.line_height = parse_length(lh).resolve(s.font_size, s.font_size) / s.font_size;
                }
                // remainder = font-family list
                std::u16string fam;
                for (size_t i = si + 1; i < toks.size(); ++i) { if (!fam.empty()) fam.push_back(u' '); fam += toks[i]; }
                if (!fam.empty()) s.font_family = fam;
            }
        }
    }
    else if (prop == u"font-weight") {
        if (lv == u"bold") s.font_weight = FontWeight::Bold;
        else if (lv == u"normal") s.font_weight = FontWeight::Normal;
        else s.font_weight = static_cast<FontWeight>(static_cast<uint16_t>(parse_float(value)));
    } else if (prop == u"line-height") {
        if (!value.empty() && value.back() != u'%' && lower(value).find(u"px") == std::u16string::npos) s.line_height = parse_float(value);
        else s.line_height = parse_length(value).resolve(s.font_size, s.font_size) / s.font_size;
    } else if (prop == u"text-align") {
        s.text_align = (lv == u"right") ? TextAlign::Right : (lv == u"center") ? TextAlign::Center : (lv == u"justify") ? TextAlign::Justify : TextAlign::Left;
    } else if (prop == u"transform") {
        s.transform = (lv == u"none") ? Transform2D{} : parse_transform(value, s.font_size);
    } else if (prop == u"flex-direction") {
        s.flex.direction = (lv == u"column") ? FlexDirection::Column : (lv == u"row-reverse") ? FlexDirection::RowReverse : (lv == u"column-reverse") ? FlexDirection::ColumnReverse : FlexDirection::Row;
    } else if (prop == u"flex-wrap") {
        s.flex.wrap = (lv == u"wrap") ? FlexWrap::Wrap : (lv == u"wrap-reverse") ? FlexWrap::WrapReverse : FlexWrap::NoWrap;
    } else if (prop == u"align-items") {
        s.flex.align_items = (lv == u"flex-start") ? AlignItems::FlexStart : (lv == u"flex-end") ? AlignItems::FlexEnd : (lv == u"center") ? AlignItems::Center : (lv == u"baseline") ? AlignItems::Baseline : AlignItems::Stretch;
    } else if (prop == u"justify-content") {
        s.flex.justify_content = (lv == u"flex-end") ? JustifyContent::FlexEnd : (lv == u"center") ? JustifyContent::Center : (lv == u"space-between") ? JustifyContent::SpaceBetween : (lv == u"space-around") ? JustifyContent::SpaceAround : (lv == u"space-evenly") ? JustifyContent::SpaceEvenly : JustifyContent::FlexStart;
    } else if (prop == u"flex-grow") s.flex.grow = parse_float(value);
    else if (prop == u"flex-shrink") s.flex.shrink = parse_float(value);
    else if (prop == u"flex-basis") s.flex.basis = parse_length(value);
    else if (prop == u"flex") {
        auto toks = split_tokens(value);
        if (toks.size() == 1 && (lv == u"none")) { s.flex.grow = 0; s.flex.shrink = 0; }
        else {
            if (toks.size() >= 1) s.flex.grow = parse_float(toks[0]);
            if (toks.size() >= 2) s.flex.shrink = parse_float(toks[1]);
            if (toks.size() >= 3) s.flex.basis = parse_length(toks[2]);
            else s.flex.basis = Length::px(0);
        }
    }
}

void parse_inline_declarations(const std::u16string& style_attr,
                               std::vector<std::pair<std::u16string, std::u16string>>& out) {
    std::u16string cur;
    std::vector<std::u16string> decls;
    int depth = 0;
    for (char16_t c : style_attr) {
        if (c == u'(') ++depth; else if (c == u')') --depth;
        if (c == u';' && depth == 0) { decls.push_back(cur); cur.clear(); } else cur.push_back(c);
    }
    decls.push_back(cur);
    for (auto& d : decls) {
        size_t colon = d.find(u':');
        if (colon == std::u16string::npos) continue;
        std::u16string p = lower(trim(d.substr(0, colon)));
        std::u16string v = trim(d.substr(colon + 1));
        if (!p.empty() && !v.empty()) out.emplace_back(p, v);
    }
}

}  // namespace

std::u16string user_agent_css() {
    return
        u"html,body,div,p,h1,h2,h3,h4,h5,h6,section,article,header,footer,nav,main,"
        u"aside,dl,dt,dd,figure,figcaption,blockquote,pre,hr,address,fieldset,form,"
        u"center,details,summary,dialog,output,legend,hgroup,search,div,article"
        u"{display:block;}"
        u"center{text-align:center;}"
        u"input,textarea,select,button{font-family:inherit;font-size:13px;}"
        u"textarea{display:inline-block;border:1px solid #767676;background:#fff;padding:2px;width:200px;height:40px;color:#000;}"
        u"select{display:inline-block;border:1px solid #767676;background:#fff;padding:1px 4px;height:22px;color:#000;white-space:nowrap;overflow:hidden;}"
        u"option,optgroup{display:none;}"  // native select shows only the chosen option as a label
        u"button{display:inline-block;border:1px solid #767676;background:#efefef;padding:2px 10px;color:#000;}"
        u"img,svg,canvas,video{display:inline-block;}"
        u"li{display:list-item;}"
        u"ul,ol,menu{display:block;}"
        u"table{display:table;border-collapse:separate;}"
        u"tr{display:table-row;}"
        u"thead,tbody,tfoot{display:table-row-group;}"
        u"td,th{display:table-cell;padding:1px;}"
        u"span,a,b,i,em,strong,small,code,label,abbr,cite,q,sub,sup,mark,u,s,big,tt,kbd,samp,var,time,wbr{display:inline;}"
        u"head,script,style,title,meta,link,base,noscript,template{display:none;}"
        u"body{margin:8px;}"
        u"h1{font-size:32px;font-weight:bold;margin-top:21px;margin-bottom:21px;}"
        u"h2{font-size:24px;font-weight:bold;margin-top:20px;margin-bottom:20px;}"
        u"h3{font-size:19px;font-weight:bold;margin-top:18px;margin-bottom:18px;}"
        u"h4{font-size:16px;font-weight:bold;margin-top:21px;margin-bottom:21px;}"
        u"h5{font-size:13px;font-weight:bold;margin-top:22px;margin-bottom:22px;}"
        u"h6{font-size:11px;font-weight:bold;margin-top:24px;margin-bottom:24px;}"
        u"p{margin-top:16px;margin-bottom:16px;}"
        u"blockquote{margin-left:40px;margin-right:40px;}"
        u"strong,b{font-weight:bold;}"
        u"em,i,cite,var{font-style:italic;}"
        u"u,ins{text-decoration:underline;}"
        u"s,strike,del{text-decoration:line-through;}"
        u"a{color:#0000ee;text-decoration:underline;}"
        u"code,kbd,samp,pre,tt{font-family:monospace;}"
        u"pre{white-space:pre;margin-top:13px;margin-bottom:13px;}"
        u"th{font-weight:bold;text-align:center;}"
        u"ul,menu{list-style-type:disc;margin-top:16px;margin-bottom:16px;padding-left:40px;}"
        u"ol{list-style-type:decimal;margin-top:16px;margin-bottom:16px;padding-left:40px;}"
        u"hr{margin-top:8px;margin-bottom:8px;border-width:1px;}";
}

void StyleResolver::add_stylesheet(const StyleSheet& sheet, Origin origin) {
    sheets_.push_back(sheet);
    const StyleSheet& stored = sheets_.back();
    for (const Rule& rule : stored.rules) {
        for (const Selector& sel : rule.selectors) {
            ComplexSelector cs = parse_selector(sel.text);
            if (cs.valid) rules_.push_back(MatchableRule{std::move(cs), &rule, origin, order_counter_++});
        }
    }
}

ComputedStyle StyleResolver::compute(Document& doc, malibu::NodeHandle node, const ComputedStyle& parent) {
    ComputedStyle style = ComputedStyle::initial();
    // Inherit inherited properties + custom properties from the parent.
    style.color = parent.color;
    style.svg_fill = parent.svg_fill;
    style.svg_fill_current_color = parent.svg_fill_current_color;
    style.svg_fill_specified = parent.svg_fill_specified;
    style.font_size = parent.font_size;
    style.font_family = parent.font_family;
    style.font_weight = parent.font_weight;
    style.line_height = parent.line_height;
    style.text_align = parent.text_align;
    style.visibility = parent.visibility;
    style.font_style = parent.font_style;
    style.white_space = parent.white_space;
    style.text_transform = parent.text_transform;
    style.list_style = parent.list_style;
    style.custom_props = parent.custom_props;

    // Collect cascade entries that match this node (and whose @media applies).
    std::vector<CascadeEntry> entries;
    // SVG presentation attributes participate at author origin with zero
    // specificity, below stylesheet rules and inline style.
    if (const NodeCore* c = doc.core(node)) {
        for (const auto& [name, value] : c->attributes) {
            if (name == u"fill") {
                apply_property(style, parent, u"fill", value);
                break;
            }
        }
    }
    for (const MatchableRule& mr : rules_) {
        if (!mr.rule->media.empty() && !media_matches(mr.rule->media, viewport_w_, viewport_h_)) continue;
        if (matches(doc, node, mr.selector)) {
            for (const Declaration& d : mr.rule->declarations)
                entries.push_back(CascadeEntry{&d, mr.origin, mr.selector.specificity, mr.order});
        }
    }
    // Inline style attribute (highest origin).
    std::vector<Declaration> inline_decls;
    if (const NodeCore* c = doc.core(node)) {
        for (auto& [k, v] : c->attributes) {
            if (k == u"style") {
                std::vector<std::pair<std::u16string, std::u16string>> kv;
                parse_inline_declarations(v, kv);
                for (auto& [p, val] : kv) inline_decls.push_back(Declaration{p, val, false, {}});
                break;
            }
        }
    }
    for (const Declaration& d : inline_decls)
        entries.push_back(CascadeEntry{&d, Origin::Inline, Specificity{1, 0, 0, }, order_counter_++});

    // Winner per property.
    std::unordered_map<std::u16string, CascadeEntry> winners;
    for (const CascadeEntry& e : entries) {
        auto it = winners.find(e.decl->property);
        if (it == winners.end() || e.wins_over(it->second)) winners[e.decl->property] = e;
    }

    // Apply custom properties first (so var() resolves), then normal properties.
    for (auto& [prop, e] : winners)
        if (prop.size() > 2 && prop[0] == u'-' && prop[1] == u'-')
            style.custom_props[prop] = substitute_vars(e.decl->value, style.custom_props);
    for (auto& [prop, e] : winners) {
        if (prop.size() > 2 && prop[0] == u'-' && prop[1] == u'-') continue;
        std::u16string value = substitute_vars(e.decl->value, style.custom_props);
        apply_property(style, parent, prop, value);
    }
    if (style.svg_fill_current_color) style.svg_fill = style.color;

    // Blockification (CSS Display §2.7): floated / absolutely-positioned elements
    // compute their display as block-level. Without this, an absolutely-positioned
    // <span> (e.g. Google's #footer) stays inline and collapses its block content
    // to zero width.
    if (style.float_ != FloatType::None ||
        style.position == PositionType::Absolute || style.position == PositionType::Fixed) {
        if (style.display == DisplayType::Inline || style.display == DisplayType::InlineBlock)
            style.display = DisplayType::Block;
        else if (style.display == DisplayType::InlineFlex) style.display = DisplayType::Flex;
        else if (style.display == DisplayType::InlineGrid) style.display = DisplayType::Grid;
        else if (style.display == DisplayType::ListItem) style.display = DisplayType::Block;
    }

    // z-index only establishes a stacking level on positioned boxes and on
    // flex/grid items (CSS2 §9.9.1 / CSS Flexbox §5.4). A static, non-item box
    // must ignore z-index — otherwise a rule like `body{background:white;
    // z-index:1}` would sort the body's background above all z=0 content and
    // paint over every bit of text on the page (real bug: old.reddit).
    {
        bool flex_grid_item = parent.display == DisplayType::Flex || parent.display == DisplayType::InlineFlex
                           || parent.display == DisplayType::Grid || parent.display == DisplayType::InlineGrid;
        if (style.has_z_index && style.position == PositionType::Static && !flex_grid_item)
            style.has_z_index = false;
    }
    return style;
}

void StyleResolver::resolve_element(Document& doc, malibu::NodeHandle node, const ComputedStyle& parent) {
    const NodeCore* c = doc.core(node);
    if (!c) return;
    const ComputedStyle* this_style = &parent;
    if (c->node_type == malibu::dom::kElementNode) {
        pool_.push_back(compute(doc, node, parent));
        ComputedStyle& cs = pool_.back();
        if (NodeCore* mut = doc.core(node)) mut->computed_style = &cs;
        this_style = &cs;
    }
    // Children inherit from this element's style (or the inherited parent for non-elements).
    std::vector<malibu::NodeHandle> kids;
    if (const NodeCore* cc = doc.core(node)) kids = cc->children;
    for (malibu::NodeHandle child : kids) resolve_element(doc, child, *this_style);
}

void StyleResolver::resolve(Document& doc) {
    ComputedStyle root = ComputedStyle::initial();
    root.display = DisplayType::Block;
    const NodeCore* r = doc.core(doc.root());
    if (!r) return;
    std::vector<malibu::NodeHandle> kids = r->children;
    for (malibu::NodeHandle child : kids) resolve_element(doc, child, root);
}

void StyleResolver::resolve_subtree(Document& doc, malibu::NodeHandle node) {
    const NodeCore* c = doc.core(node);
    if (!c) return;
    ComputedStyle parent = ComputedStyle::initial();
    parent.display = DisplayType::Block;
    if (!c->parent.is_null()) {
        if (const NodeCore* p = doc.core(c->parent))
            if (p->computed_style) parent = *p->computed_style;
    }
    resolve_element(doc, node, parent);
}

const ComputedStyle* StyleResolver::style_for(Document& doc, malibu::NodeHandle node) const {
    const NodeCore* c = doc.core(node);
    return c ? c->computed_style : nullptr;
}

} // namespace malibu::css
