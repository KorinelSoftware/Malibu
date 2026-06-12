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
#include "malibu/host/curl_resource_loader.h"

#include <SDL.h>

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
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");

    int win_w = 1280, win_h = 900;
    SDL_Window* win = SDL_CreateWindow("Malibu Browser", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       win_w, win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_Texture* tex = nullptr;

    malibu::view::View view;
    malibu::host::CurlResourceLoader loader;
    view.set_socket_handler(
        [&loader](int id, const std::string& url,
                  const std::string& data, int kind) {
            loader.websocket_command(id, url, data, kind);
        });
    view.set_fetch_handler([&loader](
        const malibu::network::FetchRequest& request,
        malibu::network::FetchResponse& out) -> bool {
        return loader.fetch(request, out);
    });
    view.set_diagnostic_handler([](
        const malibu::view::LoadDiagnostic& diagnostic) {
        std::fprintf(stderr, "malibu-browser: %s: %s\n",
                     diagnostic.url.c_str(), diagnostic.message.c_str());
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
        malibu::network::FetchResponse page;
        if (!loader.fetch(target, page)) {
            status = "failed: " + loader.last_error();
            return;
        }
        if (page.status >= 400) {
            status = "HTTP " + std::to_string(page.status) + ": " + target;
            return;
        }
        const std::string base = page.url.empty() ? target : page.url;
        loader.set_referrer(base);
        view.load_html(std::string(page.body.begin(), page.body.end()), base);
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
    uint64_t last_tick = SDL_GetTicks64();
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

        // Pump work ready at the real browser clock without draining persistent
        // intervals forever.
        uint64_t now_tick = SDL_GetTicks64();
        malibu::host::CurlResourceLoader::SocketEvent socket_event;
        while (loader.poll_websocket_event(socket_event)) {
            switch (socket_event.type) {
                case malibu::host::CurlResourceLoader::SocketEventType::Open:
                    view.socket_open(socket_event.id);
                    break;
                case malibu::host::CurlResourceLoader::SocketEventType::Message:
                    view.socket_message(
                        socket_event.id, socket_event.data);
                    break;
                case malibu::host::CurlResourceLoader::SocketEventType::Close:
                    view.socket_close(
                        socket_event.id, socket_event.code,
                        socket_event.reason);
                    break;
            }
            dirty = true;
        }
        view.run_tasks(now_tick - last_tick);
        last_tick = now_tick;

        if (dirty || !tex) {
            if (!tex) {
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, win_w, win_h);
                if (!tex) {
                    std::fprintf(stderr, "SDL_CreateTexture: %s\n",
                                 SDL_GetError());
                    running = false;
                    break;
                }
            }

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
    SDL_Quit();
    return 0;
}
