// compat/expectation_layer.cpp
// Three-level compatibility responder. Truthful by default: a browser-identity
// value is returned only for a detected/observed probe, and never impersonates
// a browser Malibu is not (Opera / Firefox / IE probes stay undefined).

#include "malibu/compat/expectation_layer.h"
#include "malibu/diagnostics/diagnostic_log.h"

#include <string>

namespace malibu::compat {
namespace {

// The compatibility value Malibu exposes for a probe, or "" for undefined.
std::string default_compat_value(const std::string& probe_id) {
    if (probe_id == "window.chrome")        return "{}";  // minimal presence
    if (probe_id == "navigator.userAgent")
        return "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
               "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36 Malibu/0.1";
    if (probe_id == "navigator.vendor")     return "Google Inc.";
    // window.opr (Opera), window.InstallTrigger (Firefox),
    // document.documentMode (IE), window.StyleMedia (IE/Edge):
    // Malibu is none of these, so do not fake their identity.
    return "";
}

ProbeResponse make_response(const std::string& probe_id) {
    std::string v = default_compat_value(probe_id);
    return ProbeResponse{v, !v.empty()};
}

bool is_known(const std::string& probe_id) {
    for (const auto& p : StaticPreProfiler::known_probes())
        if (p == probe_id) return true;
    return false;
}

}  // namespace

void ExpectationLayer::configure_from_probe_report(const ProbeReport& report) {
    for (const auto& probe : report.detected_probes) {
        static_pre_profile_[probe] = make_response(probe);
    }
    current_level_ = CompatLevel::StaticPreProfile;
}

void ExpectationLayer::note_first_execute(uint64_t now_ms) {
    if (!first_execute_recorded_) {
        first_execute_timestamp_ms_ = now_ms;
        first_execute_recorded_ = true;
    }
}

void ExpectationLayer::mark_api_called(const std::string& api) {
    called_apis_.insert(api);
}

ProbeResponse ExpectationLayer::respond(const std::string& probe_id, uint64_t now_ms) {
    uint64_t elapsed = first_execute_recorded_ && now_ms >= first_execute_timestamp_ms_
                           ? now_ms - first_execute_timestamp_ms_
                           : 0;
    current_level_ = !first_execute_recorded_ ? CompatLevel::StaticPreProfile
                     : (elapsed < kAdaptiveThresholdMs ? CompatLevel::EarlyRuntimeProfile
                                                       : CompatLevel::AdaptiveRuntime);

    ProbeResponse resp;
    auto detected = static_pre_profile_.find(probe_id);
    if (detected != static_pre_profile_.end()) {
        resp = detected->second;                 // detected statically
    } else if (is_known(probe_id)) {
        resp = make_response(probe_id);           // observed at runtime
    } else {
        resp = ProbeResponse{};                   // unknown probe → undefined
    }

    auto& level_map = (current_level_ == CompatLevel::AdaptiveRuntime)
                          ? adaptive_runtime_
                          : early_runtime_profile_;
    auto it = level_map.find(probe_id);
    ProbeResponse old = it != level_map.end() ? it->second : ProbeResponse{};
    if (!(old == resp)) {
        log_change(probe_id, old, resp, elapsed);
    }
    level_map[probe_id] = resp;
    return resp;
}

CompatLevel ExpectationLayer::current_level() const { return current_level_; }

ProbeResponse ExpectationLayer::query_level(CompatLevel lvl, const std::string& probe_id) const {
    const std::unordered_map<std::string, ProbeResponse>* map = nullptr;
    switch (lvl) {
        case CompatLevel::StaticPreProfile:    map = &static_pre_profile_; break;
        case CompatLevel::EarlyRuntimeProfile: map = &early_runtime_profile_; break;
        case CompatLevel::AdaptiveRuntime:     map = &adaptive_runtime_; break;
    }
    auto it = map->find(probe_id);
    return it != map->end() ? it->second : ProbeResponse{};
}

void ExpectationLayer::log_change(const std::string& probe_id, const ProbeResponse& old_val,
                                  const ProbeResponse& new_val, uint64_t elapsed_ms) {
    MALIBU_LOG(INFO, "compat",
               "probe '" + probe_id + "' '" + old_val.value + "' -> '" + new_val.value +
                   "' at +" + std::to_string(elapsed_ms) + "ms");
}

} // namespace malibu::compat
