#pragma once
// core/include/malibu/capability_manifest/capability_manifest.h
// Feature capability registry (Task 4 / Requirement 19.4, 19.5).
//
// The manifest is embedded at compile time and parsed once at first access
// using simdjson. A feature listed as "supported" returns true; anything not
// listed (or listed as unsupported) returns false, so callers can throw
// NotSupportedError / return undefined rather than silently misbehaving.

#include <string>
#include <string_view>
#include <unordered_map>

namespace malibu::capability_manifest {

class CapabilityManifest {
public:
    static CapabilityManifest& instance();

    // True iff `feature` is present in the manifest with value true.
    [[nodiscard]] bool is_supported(std::string_view feature) const;

    // Number of features declared in the manifest (supported or not).
    [[nodiscard]] size_t feature_count() const noexcept { return features_.size(); }

    // True iff `feature` is present in the manifest (regardless of value).
    [[nodiscard]] bool is_known(std::string_view feature) const;

private:
    CapabilityManifest();  // parses the embedded manifest

    std::unordered_map<std::string, bool> features_;
};

} // namespace malibu::capability_manifest
