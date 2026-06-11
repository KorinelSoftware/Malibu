// tools/malibu_render.cpp
// Headless screenshot tool: render an HTML file OR a live URL to a PNG.
//
//   malibu_render <input.html|http(s)://url> <output.png> [width] [height]
//
// For a URL, the page and all its subresources (CSS/JS/images) are fetched by
// the *host* via curl — TLS lives in the host process, so the engine itself
// stays transport/TLS-free (it only ever receives bytes through a request
// handler). This is the legitimate "host provides bytes" model.

#include "malibu/view/view.h"
#include "malibu/image/png.h"

#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {
// Fetches a URL via the system curl (host-side TLS). Returns false on failure.
bool curl_fetch(const std::string& url, std::vector<uint8_t>& body) {
    // Reject shell-significant chars (this is a local dev tool, but be safe).
    for (char c : url) if (c == '\'' || c == '`' || c == '\n' || c == '\\') return false;
    std::string cmd = "curl -sL --max-time 25 -A "
        "'Mozilla/5.0 (X11; Linux x86_64) Malibu/0.1' --compressed -- '" + url + "'";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    std::array<char, 65536> buf;
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0)
        body.insert(body.end(), buf.data(), buf.data() + n);
    int rc = pclose(p);
    return rc == 0 && !body.empty();
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <input.html|url> <output.png> [width] [height]\n", argv[0]);
        return 2;
    }
    const std::string input = argv[1];
    const std::string output = argv[2];
    int width = argc > 3 ? std::atoi(argv[3]) : 1024;
    // height: a number, or "full"/0 to capture the entire page in one tall image.
    bool full = argc > 4 && (std::string(argv[4]) == "full" || std::string(argv[4]) == "0");
    int height = argc > 4 ? std::atoi(argv[4]) : 768;
    if (width <= 0) width = 1024;
    if (!full && height <= 0) height = 768;

    bool is_url = input.rfind("http://", 0) == 0 || input.rfind("https://", 0) == 0;

    malibu::view::View view;

    if (is_url) {
        // Host fetches the page + every subresource the engine asks for.
        view.set_request_handler([](const std::string& url, malibu::network::FetchResponse& out) -> bool {
            std::vector<uint8_t> body;
            if (!curl_fetch(url, body)) return false;
            out.body = std::move(body);
            out.status = 200;
            return true;
        });
        std::vector<uint8_t> page;
        if (!curl_fetch(input, page)) { std::fprintf(stderr, "malibu_render: fetch failed: %s\n", input.c_str()); return 1; }
        std::string html(page.begin(), page.end());
        if (!view.load_html(html, input)) { std::fprintf(stderr, "malibu_render: load failed\n"); return 1; }
        std::fprintf(stderr, "malibu_render: fetched %zu bytes from %s\n", page.size(), input.c_str());
    } else {
        std::ifstream f(input, std::ios::binary);
        if (!f) { std::fprintf(stderr, "malibu_render: cannot open %s\n", input.c_str()); return 1; }
        std::ostringstream ss; ss << f.rdbuf();
        // Local subresources (img/css/js): read files relative to the page dir.
        std::string dir = input.substr(0, input.find_last_of('/') + 1);
        view.set_request_handler([dir](const std::string& url, malibu::network::FetchResponse& out) -> bool {
            std::string path = url;
            auto fp = path.find("file://"); if (fp == 0) path = path.substr(7);
            if (!path.empty() && path[0] != '/') path = dir + path;     // relative to page
            std::ifstream rf(path, std::ios::binary);
            if (!rf) return false;
            std::ostringstream rs; rs << rf.rdbuf();
            std::string body = rs.str();
            out.body.assign(body.begin(), body.end()); out.status = 200; return true;
        });
        if (!view.load_html(ss.str(), "file://" + input)) { std::fprintf(stderr, "malibu_render: load failed\n"); return 1; }
    }

    if (full) {  // whole page in one tall image (capped for memory)
        height = static_cast<int>(view.page_height(width)) + 20;
        if (height < 200) height = 200;
        if (height > 24000) height = 24000;
        std::fprintf(stderr, "malibu_render: full-page height = %d\n", height);
    }
    malibu::render::Framebuffer fb = view.render(width, height);
    if (!malibu::image::write_png_rgba(output, fb.rgba.data(), fb.width, fb.height)) {
        std::fprintf(stderr, "malibu_render: failed to write %s\n", output.c_str());
        return 1;
    }
    if (full) {  // also emit readable viewport slices (scroll captures)
        const int vh = 900;
        std::string base = output; auto dot = base.rfind('.'); if (dot != std::string::npos) base = base.substr(0, dot);
        for (int s = 0, y = 0; y < fb.height && s < 12; ++s, y += vh) {
            int sh = std::min(vh, fb.height - y);
            std::vector<uint8_t> slice(static_cast<size_t>(fb.width) * sh * 4);
            std::memcpy(slice.data(), fb.rgba.data() + static_cast<size_t>(y) * fb.width * 4,
                        slice.size());
            std::string sp = base + "_s" + std::to_string(s) + ".png";
            malibu::image::write_png_rgba(sp, slice.data(), fb.width, sh);
        }
        std::fprintf(stderr, "malibu_render: wrote scroll slices %s_sN.png\n", base.c_str());
    }
    std::printf("malibu_render: %s -> %s (%dx%d)\n", input.c_str(), output.c_str(), width, height);
    return 0;
}
