// core/capability_manifest/capability_manifest.cpp
// Parses the embedded capability manifest once, using simdjson.

#include "malibu/capability_manifest/capability_manifest.h"
#include "malibu/capability_manifest/capability_manifest_data.h"  // generated

#include <simdjson.h>

#include <string>

namespace malibu::capability_manifest {

CapabilityManifest& CapabilityManifest::instance() {
    static CapabilityManifest m;
    return m;
}

CapabilityManifest::CapabilityManifest() {
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto error = parser.parse(kEmbeddedManifestJson,
                              std::char_traits<char>::length(kEmbeddedManifestJson))
                     .get(doc);
    if (error) {
        return;  // empty manifest: every query returns false (correctness-first)
    }

    simdjson::dom::object obj;
    if (doc.get_object().get(obj)) {
        return;
    }

    for (auto field : obj) {
        // Skip non-boolean metadata fields (e.g. "_comment").
        bool value;
        if (field.value.get_bool().get(value)) {
            continue;
        }
        features_.emplace(std::string(field.key), value);
    }
}

bool CapabilityManifest::is_supported(std::string_view feature) const {
    auto it = features_.find(std::string(feature));
    return it != features_.end() && it->second;
}

bool CapabilityManifest::is_known(std::string_view feature) const {
    return features_.find(std::string(feature)) != features_.end();
}

} // namespace malibu::capability_manifest
