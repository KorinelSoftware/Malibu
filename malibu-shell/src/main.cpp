// malibu-shell/src/main.cpp
// An interactive SDL2 mini-browser on top of the Malibu engine. It owns a real
// window, fetches pages + subresources over TLS via libcurl (the engine stays
// transport-free), and drives the View for render / scroll / click / hover /
// type. Chrome (address bar + buttons) is drawn with Malibu's own text engine.
//
//   malibu-browser [url]

#include "malibu/view/view.h"
#include "malibu/text/font.h"
#include "malibu/text/glyph_drawer.h"
#include "malibu/render/raster/software_rasterizer.h"
#include "malibu/image/png.h"

#include <SDL.h>
#include <curl/curl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using malibu::render::Framebuffer;
using malibu::render::Color;

namespace {
constexpr int kChromeH = 44;   // top toolbar height

// ---- UTF conversions (host side) ----
std::u16string widen(const std::string& s) {
    std::u16string out; out.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        char32_t cp; int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { out.push_back(0xFFFD); ++i; continue; }
        if (i + (size_t)extra >= n) { out.push_back(0xFFFD); break; }
        for (int k = 1; k <= extra; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += extra + 1;
        if (cp <= 0xFFFF) out.push_back((char16_t)cp);
        else { cp -= 0x10000; out.push_back((char16_t)(0xD800 + (cp >> 10))); out.push_back((char16_t)(0xDC00 + (cp & 0x3FF))); }
    }
    return out;
}
std::string narrow(const std::u16string& s) {
    std::string r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char32_t cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (cp < 0x80) r.push_back((char)cp);
        else if (cp < 0x800) { r.push_back((char)(0xC0 | (cp >> 6))); r.push_back((char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { r.push_back((char)(0xE0 | (cp >> 12))); r.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); r.push_back((char)(0x80 | (cp & 0x3F))); }
        else { r.push_back((char)(0xF0 | (cp >> 18))); r.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); r.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); r.push_back((char)(0x80 | (cp & 0x3F))); }
    }
    return r;
}

// ---- libcurl fetch (TLS in the host) ----
size_t write_cb(void* p, size_t s, size_t n, void* u) {
    auto* v = static_cast<std::vector<uint8_t>*>(u);
    v->insert(v->end(), (uint8_t*)p, (uint8_t*)p + s * n);
    return s * n;
}
bool fetch(const std::string& url, std::vector<uint8_t>& body) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Malibu/0.1");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return r == CURLE_OK && !body.empty();
}

// ---- address-bar logic ----
std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o.push_back((char)c);
        else if (c == ' ') o.push_back('+');
        else { o.push_back('%'); o.push_back(hex[c >> 4]); o.push_back(hex[c & 0xF]); }
    }
    return o;
}
// Turn the address-bar text into a navigable URL: keep URLs, prepend https:// to
// bare domains, otherwise search DuckDuckGo's no-JS HTML endpoint.
std::string to_target(std::string in) {
    size_t a = in.find_first_not_of(" \t"); if (a == std::string::npos) return in;
    in = in.substr(a);
    if (in.rfind("http://", 0) == 0 || in.rfind("https://", 0) == 0) return in;
    bool looks_like_host = in.find(' ') == std::string::npos &&
                           (in.find('.') != std::string::npos || in.rfind("localhost", 0) == 0);
    if (looks_like_host) return "https://" + in;
    return "https://html.duckduckgo.com/html/?q=" + url_encode(in);
}
// Resolve a possibly-relative href against the current page URL.
std::string resolve(const std::string& base, std::string ref) {
    size_t a = ref.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return base;
    ref = ref.substr(a);
    if (ref.rfind("//", 0) == 0) return (base.rfind("http://", 0) == 0 ? "http:" : "https:") + ref;
    if (ref.find("://") != std::string::npos && ref.find("://") < 12) return ref;
    auto s = base.find("://");
    if (s == std::string::npos) return ref;
    if (!ref.empty() && ref[0] == '/') {
        auto slash = base.find('/', s + 3);
        return (slash == std::string::npos ? base : base.substr(0, slash)) + ref;
    }
    auto slash = base.find_last_of('/');
    return (slash == std::string::npos || slash < s + 3 ? base + "/" : base.substr(0, slash + 1)) + ref;
}

// ---- minimal raster helpers for the chrome strip ----
void fill(Framebuffer& fb, int x0, int y0, int w, int h, Color c) {
    for (int y = y0; y < y0 + h && y < fb.height; ++y) {
        if (y < 0) continue;
        for (int x = x0; x < x0 + w && x < fb.width; ++x) {
            if (x < 0) continue;
            size_t i = ((size_t)y * fb.width + x) * 4;
            fb.rgba[i] = c.r; fb.rgba[i + 1] = c.g; fb.rgba[i + 2] = c.b; fb.rgba[i + 3] = 255;
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    curl_global_init(CURL_GLOBAL_DEFAULT);

    int win_w = 1280, win_h = 900;
    SDL_Window* win = SDL_CreateWindow("Malibu Browser", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       win_w, win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture* tex = nullptr;

    malibu::view::View view;
    view.set_request_handler([](const std::string& url, malibu::network::FetchResponse& out) -> bool {
        std::vector<uint8_t> b;
        if (!fetch(url, b)) return false;
        out.body = std::move(b); out.status = 200; return true;
    });

    malibu::text::FontSystem fonts;
    malibu::text::FreeTypeTextDrawer chrome_text(fonts, "sans-serif");

    std::string url_text = argc > 1 ? argv[1] : "https://lite.cnn.com";
    std::string status;
    float scroll_y = 0, page_h = 0;
    bool url_focused = false, dirty = true;

    auto content_h = [&]() { return std::max(1, win_h - kChromeH); };
    auto navigate = [&](const std::string& target) {
        status = "loading " + target + " ...";
        std::vector<uint8_t> page;
        if (!fetch(target, page)) { status = "failed to load " + target; return; }
        view.load_html(std::string(page.begin(), page.end()), target);
        url_text = view.current_url();
        scroll_y = 0;
        page_h = view.page_height(win_w);
        status.clear();
        dirty = true;
    };
    navigate(to_target(url_text));

    auto draw_text = [&](Framebuffer& fb, const std::string& s, float x, float y, Color col, float size) {
        malibu::render::PaintText pt; pt.x = x; pt.y = y; pt.text = widen(s); pt.color = col; pt.font_size = size;
        chrome_text.draw_text(fb, pt, malibu::render::Transform2D{}, 1.0f, malibu::render::ClipRect{});
    };

    SDL_StartTextInput();
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: running = false; break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    win_w = e.window.data1; win_h = e.window.data2;
                    if (tex) { SDL_DestroyTexture(tex); tex = nullptr; }
                    page_h = view.page_height(win_w); dirty = true;
                }
                break;
            case SDL_MOUSEWHEEL: {
                float maxs = std::max(0.0f, page_h - content_h());
                scroll_y = std::min(maxs, std::max(0.0f, scroll_y - e.wheel.y * 60.0f));
                dirty = true;
                break;
            }
            case SDL_MOUSEMOTION:
                if (!url_focused && e.motion.y >= kChromeH) {
                    if (view.set_hover(e.motion.x, e.motion.y - kChromeH + scroll_y)) dirty = true;
                }
                break;
            case SDL_MOUSEBUTTONDOWN: {
                if (e.button.y < kChromeH) {
                    if (e.button.x < 36) { if (view.go_back()) { url_text = view.current_url(); scroll_y = 0; page_h = view.page_height(win_w); } dirty = true; }
                    else if (e.button.x < 68) { if (view.go_forward()) { url_text = view.current_url(); scroll_y = 0; page_h = view.page_height(win_w); } dirty = true; }
                    else if (e.button.x < 100) { navigate(view.current_url()); }
                    else url_focused = true;   // clicked the address field
                    dirty = true;
                } else {
                    url_focused = false;
                    float dx = e.button.x, dy = e.button.y - kChromeH + scroll_y;
                    malibu::NodeHandle hit = view.dispatch_mouse(dx, dy, "click");
                    // Follow a link if we clicked inside an <a href>.
                    auto& doc = view.document();
                    for (malibu::NodeHandle n = hit; doc.core(n); n = doc.core(n)->parent) {
                        auto* c = doc.core(n);
                        if (c->tag_name == u"a") {
                            auto href = view.tree().get_attribute(n, u"href");
                            if (href && !href->empty() && href->rfind(u"javascript:", 0) != 0 && (*href)[0] != u'#') {
                                navigate(resolve(view.current_url(), narrow(*href)));
                            }
                            break;
                        }
                    }
                    page_h = view.page_height(win_w);
                    dirty = true;
                }
                break;
            }
            case SDL_TEXTINPUT:
                if (url_focused) { url_text += e.text.text; dirty = true; }
                else { view.dispatch_key(e.text.text, true); page_h = view.page_height(win_w); dirty = true; }
                break;
            case SDL_KEYDOWN: {
                SDL_Keycode k = e.key.keysym.sym;
                if (url_focused) {
                    if (k == SDLK_RETURN) { url_focused = false; navigate(to_target(url_text)); }
                    else if (k == SDLK_BACKSPACE && !url_text.empty()) { url_text.pop_back(); dirty = true; }
                    else if (k == SDLK_ESCAPE) { url_focused = false; url_text = view.current_url(); dirty = true; }
                } else {
                    if (k == SDLK_F5) navigate(view.current_url());
                    else if (k == SDLK_BACKSPACE) { view.dispatch_key("Backspace", false); dirty = true; }
                    else if (k == SDLK_RETURN) { view.dispatch_key("Enter", false); dirty = true; }
                    else if (k == SDLK_l && (e.key.keysym.mod & KMOD_CTRL)) url_focused = true;
                    else if (k == SDLK_LEFT && (e.key.keysym.mod & KMOD_ALT)) { if (view.go_back()) { url_text = view.current_url(); scroll_y = 0; page_h = view.page_height(win_w); dirty = true; } }
                }
                break;
            }
            default: break;
            }
        }

        // Pump JS timers/promises/rAF; if anything changed layout we re-render.
        view.run_tasks();

        if (dirty || !tex) {
            if (!tex)
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, win_w, win_h);

            Framebuffer page = view.render(win_w, content_h(), scroll_y);

            // Compose window framebuffer: chrome strip + page.
            Framebuffer wfb(win_w, win_h);
            wfb.clear({255, 255, 255, 255});
            for (int y = 0; y < content_h() && y < page.height; ++y)
                std::copy_n(&page.rgba[(size_t)y * page.width * 4], (size_t)win_w * 4,
                            &wfb.rgba[(size_t)(y + kChromeH) * win_w * 4]);

            // Chrome: bar, buttons, address field, URL text, status.
            fill(wfb, 0, 0, win_w, kChromeH, {236, 238, 240, 255});
            fill(wfb, 0, kChromeH - 1, win_w, 1, {200, 200, 200, 255});
            draw_text(wfb, "<", 14, 12, {60, 60, 60, 255}, 20);
            draw_text(wfb, ">", 46, 12, {60, 60, 60, 255}, 20);
            draw_text(wfb, "R", 78, 13, {60, 60, 60, 255}, 17);
            fill(wfb, 104, 7, win_w - 114, kChromeH - 14, url_focused ? Color{255, 255, 255, 255} : Color{248, 248, 248, 255});
            fill(wfb, 104, 7, win_w - 114, 1, {180, 180, 180, 255});
            std::string shown = url_focused ? url_text + "|" : (status.empty() ? url_text : status);
            draw_text(wfb, shown, 114, 13, {30, 30, 30, 255}, 15);

            SDL_UpdateTexture(tex, nullptr, wfb.rgba.data(), win_w * 4);
            dirty = false;

            // Debug aid: MALIBU_DUMP=path.png dumps the first composed frame + exits.
            if (const char* dump = std::getenv("MALIBU_DUMP")) {
                malibu::image::write_png_rgba(dump, wfb.rgba.data(), wfb.width, wfb.height);
                std::fprintf(stderr, "malibu-shell: dumped %s (%dx%d)\n", dump, wfb.width, wfb.height);
                running = false;
            }
        }
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);
        SDL_Delay(8);
    }

    if (tex) SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    curl_global_cleanup();
    SDL_Quit();
    return 0;
}
