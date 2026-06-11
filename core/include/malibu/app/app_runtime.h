#pragma once
// core/include/malibu/app/app_runtime.h
// Malibu App Runtime (Task 32): launches an installed web app (PWA) from its
// Web App Manifest into a MalibuView in standalone mode.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "malibu/app/manifest.h"
#include "malibu/view/view.h"

namespace malibu::app {

struct LaunchResult {
    bool                        ok = false;
    std::string                 error;
    AppManifest                 manifest;
    bool                        standalone = false;   // window omits browser chrome
    std::string                 window_title;
    std::string                 icon;                 // best-fit icon src
    std::unique_ptr<view::View> view;                 // the loaded app view
};

class AppRuntime {
public:
    // Resolves an app resource (e.g. start_url) to its content. Returns nullopt
    // when the resource cannot be loaded.
    using ResourceLoader = std::function<std::optional<std::string>(const std::string& url)>;

    // Parses the manifest, validates start_url, and launches the app. On a
    // missing/invalid start_url the launch aborts with an error (Req 16.6).
    LaunchResult launch(std::string_view manifest_json, const ResourceLoader& loader);
};

} // namespace malibu::app
