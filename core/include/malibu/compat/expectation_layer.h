#pragma once
// core/include/malibu/compat/expectation_layer.h
// Expectation Layer (Task 20 / Requirement 10).
//
// Three-level compatibility system. It never returns a browser-identity value
// for a probe unless that probe was detected statically or observed at runtime;
// undetected probes return undefined / the Malibu-native value.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "malibu/compat/static_pre_profiler.h"

namespace malibu::compat {

enum class CompatLevel : uint8_t {
    StaticPreProfile    = 0,
    EarlyRuntimeProfile = 1,
    AdaptiveRuntime     = 2,
};

struct ProbeResponse {
    std::string value;          // empty + !active => "return undefined"
    bool        active = false; // false => native / undefined

    bool operator==(const ProbeResponse&) const noexcept = default;
};

// Elapsed time threshold (ms) separating Early and Adaptive runtime profiles.
inline constexpr uint64_t kAdaptiveThresholdMs = 500;

class ExpectationLayer {
public:
    // Step 1: configure the static pre-profile from the scan report, before any
    // bytecode executes.
    void configure_from_probe_report(const ProbeReport& report);

    // Records the timestamp of the first script execute() for this document.
    // The 500 ms threshold is measured from here (Req 10.7).
    void note_first_execute(uint64_t now_ms);

    // Responds to a probe access at time `now_ms`. Selects the level by elapsed
    // time and returns a compatibility value only for detected/observed probes.
    ProbeResponse respond(const std::string& probe_id, uint64_t now_ms);

    // Marks that the page successfully called a web API (feeds AdaptiveRuntime).
    void mark_api_called(const std::string& api);

    // Diagnostics
    [[nodiscard]] CompatLevel   current_level() const;
    [[nodiscard]] ProbeResponse query_level(CompatLevel lvl, const std::string& probe_id) const;
    [[nodiscard]] uint64_t      first_execute_ms() const noexcept { return first_execute_timestamp_ms_; }

private:
    void log_change(const std::string& probe_id, const ProbeResponse& old_val,
                    const ProbeResponse& new_val, uint64_t elapsed_ms);

    uint64_t    first_execute_timestamp_ms_ = 0;
    bool        first_execute_recorded_     = false;
    CompatLevel current_level_              = CompatLevel::StaticPreProfile;

    std::unordered_map<std::string, ProbeResponse> static_pre_profile_;
    std::unordered_map<std::string, ProbeResponse> early_runtime_profile_;
    std::unordered_map<std::string, ProbeResponse> adaptive_runtime_;
    std::unordered_set<std::string>                called_apis_;
};

} // namespace malibu::compat
