// malibu-view/src/malibu_view_c_api.cpp
// Thin C ABI wrapper over malibu::view::View.

#include "malibu_view.h"
#include "malibu/view/view.h"

#include <cstring>
#include <string>

using malibu::view::View;

namespace {
char* dup_string(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (out) { std::memcpy(out, s.data(), s.size()); out[s.size()] = '\0'; }
    return out;
}
}  // namespace

extern "C" {

MalibuView* malibu_view_create(void) { return reinterpret_cast<MalibuView*>(new View()); }
void malibu_view_destroy(MalibuView* v) { delete reinterpret_cast<View*>(v); }

int malibu_view_load_html(MalibuView* v, const char* html, const char* base_url) {
    if (!v || !html) return 0;
    return reinterpret_cast<View*>(v)->load_html(html, base_url ? base_url : "about:blank") ? 1 : 0;
}
int malibu_view_load_file(MalibuView* v, const char* path) {
    if (!v || !path) return 0;
    return reinterpret_cast<View*>(v)->load_file(path) ? 1 : 0;
}
int malibu_view_load_url(MalibuView* v, const char* url) {
    if (!v || !url) return 0;
    return reinterpret_cast<View*>(v)->load_url(url) ? 1 : 0;
}
void malibu_view_reload(MalibuView* v) { if (v) reinterpret_cast<View*>(v)->reload(); }
int  malibu_view_go_back(MalibuView* v) { return v && reinterpret_cast<View*>(v)->go_back() ? 1 : 0; }
int  malibu_view_go_forward(MalibuView* v) { return v && reinterpret_cast<View*>(v)->go_forward() ? 1 : 0; }

char* malibu_view_eval_js(MalibuView* v, const char* source) {
    if (!v || !source) return dup_string("error: invalid arguments");
    return dup_string(reinterpret_cast<View*>(v)->eval_js(source));
}
void malibu_view_free_string(char* s) { std::free(s); }

void malibu_view_post_message(MalibuView* v, const char* msg) {
    if (v && msg) reinterpret_cast<View*>(v)->post_message(msg);
}
void malibu_view_set_message_handler(MalibuView* v, MalibuMessageHandler handler, void* user) {
    if (!v) return;
    reinterpret_cast<View*>(v)->set_message_handler(
        [handler, user](const std::string& m) { if (handler) handler(m.c_str(), user); });
}

void malibu_view_set_sandbox(MalibuView* v, uint32_t flags) {
    if (v) reinterpret_cast<View*>(v)->set_sandbox(flags);
}

int malibu_view_render(MalibuView* v, int width, int height, uint8_t* out, size_t out_size) {
    if (!v || !out) return 0;
    size_t need = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (out_size < need) return 0;
    auto fb = reinterpret_cast<View*>(v)->render(width, height);
    std::memcpy(out, fb.rgba.data(), need);
    return 1;
}

void malibu_view_run_tasks(MalibuView* v) { if (v) reinterpret_cast<View*>(v)->run_tasks(); }

}  // extern "C"
