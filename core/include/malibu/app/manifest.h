#pragma once
// core/include/malibu/app/manifest.h
// Web App Manifest parsing (Task 32 / W3C Web App Manifest, Requirement 16.1).

#include <string>
#include <string_view>
#include <vector>

namespace malibu::app {

struct AppIcon {
    std::string src;
    std::string sizes;   // e.g. "192x192"
    std::string type;    // e.g. "image/png"
};

enum class DisplayMode { Browser, MinimalUi, Standalone, Fullscreen };

struct AppManifest {
    std::string             name;
    std::string             short_name;
    std::string             start_url;
    std::string             scope;
    DisplayMode             display = DisplayMode::Browser;
    std::string             theme_color;
    std::string             background_color;
    std::vector<AppIcon>    icons;

    bool                    valid = false;
    std::string             error;

    // Picks the icon whose first "WxH" size is closest to (and >=) target_px,
    // else the largest available. Returns empty src if no icons.
    [[nodiscard]] std::string best_icon(int target_px) const;
    [[nodiscard]] bool standalone() const noexcept {
        return display == DisplayMode::Standalone || display == DisplayMode::Fullscreen ||
               display == DisplayMode::MinimalUi;
    }
};

// Parses a Web App Manifest. A missing/empty start_url yields valid=false
// (Requirement 16.6).
AppManifest parse_manifest(std::string_view json);

} // namespace malibu::app
