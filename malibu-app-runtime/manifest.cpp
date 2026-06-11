// malibu-app-runtime/manifest.cpp
// Web App Manifest parsing via simdjson.

#include "malibu/app/manifest.h"

#include <simdjson.h>

#include <cstdlib>

namespace malibu::app {
namespace {
std::string str_field(simdjson::dom::object& obj, const char* key) {
    std::string_view v;
    if (!obj[key].get_string().get(v)) return std::string(v);
    return std::string();
}
int first_size(const std::string& sizes) {
    // "192x192 512x512" → 192
    try { return std::atoi(sizes.c_str()); } catch (...) { return 0; }
}
}  // namespace

std::string AppManifest::best_icon(int target_px) const {
    if (icons.empty()) return std::string();
    const AppIcon* best = nullptr;
    int best_score = -1;
    for (const AppIcon& ic : icons) {
        int sz = first_size(ic.sizes);
        // Prefer the smallest icon >= target; otherwise the largest overall.
        int score = (sz >= target_px) ? (10000 - (sz - target_px)) : sz;
        if (score > best_score) { best_score = score; best = &ic; }
    }
    return best ? best->src : icons.front().src;
}

AppManifest parse_manifest(std::string_view json) {
    AppManifest m;
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto err = parser.parse(json.data(), json.size()).get(doc);
    if (err) { m.error = "manifest is not valid JSON"; return m; }

    simdjson::dom::object obj;
    if (doc.get_object().get(obj)) { m.error = "manifest is not an object"; return m; }

    m.name = str_field(obj, "name");
    m.short_name = str_field(obj, "short_name");
    m.start_url = str_field(obj, "start_url");
    m.scope = str_field(obj, "scope");
    m.theme_color = str_field(obj, "theme_color");
    m.background_color = str_field(obj, "background_color");

    std::string display = str_field(obj, "display");
    if (display == "standalone") m.display = DisplayMode::Standalone;
    else if (display == "fullscreen") m.display = DisplayMode::Fullscreen;
    else if (display == "minimal-ui") m.display = DisplayMode::MinimalUi;
    else m.display = DisplayMode::Browser;

    simdjson::dom::array icons;
    if (!obj["icons"].get_array().get(icons)) {
        for (auto el : icons) {
            simdjson::dom::object io;
            if (el.get_object().get(io)) continue;
            AppIcon ic;
            ic.src = str_field(io, "src");
            ic.sizes = str_field(io, "sizes");
            ic.type = str_field(io, "type");
            if (!ic.src.empty()) m.icons.push_back(std::move(ic));
        }
    }

    if (m.start_url.empty()) { m.error = "manifest missing start_url"; return m; }
    m.valid = true;
    return m;
}

} // namespace malibu::app
