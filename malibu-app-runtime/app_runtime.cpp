// malibu-app-runtime/app_runtime.cpp
// Launches a PWA from its manifest into a standalone MalibuView.

#include "malibu/app/app_runtime.h"
#include "malibu/diagnostics/diagnostic_log.h"

namespace malibu::app {

LaunchResult AppRuntime::launch(std::string_view manifest_json, const ResourceLoader& loader) {
    LaunchResult result;
    result.manifest = parse_manifest(manifest_json);

    if (!result.manifest.valid) {
        result.error = result.manifest.error.empty() ? "invalid manifest" : result.manifest.error;
        MALIBU_LOG(ERROR, "app", "PWA launch aborted: " + result.error);
        return result;  // abort (Req 16.6)
    }

    // Fetch the start_url content.
    std::optional<std::string> html = loader ? loader(result.manifest.start_url) : std::nullopt;
    if (!html) {
        result.error = "could not load start_url: " + result.manifest.start_url;
        MALIBU_LOG(ERROR, "app", result.error);
        return result;
    }

    result.standalone = result.manifest.standalone();
    result.window_title = !result.manifest.name.empty() ? result.manifest.name
                                                        : result.manifest.short_name;
    result.icon = result.manifest.best_icon(192);

    result.view = std::make_unique<view::View>();
    if (!result.view->load_html(*html, result.manifest.start_url)) {
        result.error = "failed to load the app document";
        result.view.reset();
        return result;
    }
    result.ok = true;
    MALIBU_LOG(INFO, "app", "PWA launched: " + result.window_title +
                                (result.standalone ? " (standalone)" : ""));
    return result;
}

} // namespace malibu::app
