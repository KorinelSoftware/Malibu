#pragma once
// core/include/malibu/compat/static_pre_profiler.h
// Static Pre-Profiler (Task 20 / Requirement 9).
//
// Scans JS source for environment-sniffing "probes" BEFORE any bytecode runs,
// so the Expectation Layer can configure compatibility responses up front. It
// is a single-pass lightweight scanner — not a full parser — and never rewrites
// the source. Target throughput: >=20 MB/s (<=5 ms / 100 KB).

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace malibu::compat {

struct ProbeReport {
    std::vector<std::string> detected_probes;     // e.g. "window.chrome"
    bool                     tokenization_failed = false;
};

class StaticPreProfiler {
public:
    // Scans source synchronously and returns the set of detected probe ids.
    // Detects dot notation, bracket notation, and destructuring forms.
    ProbeReport scan(std::u16string_view source);

    // The canonical probe ids this profiler recognises.
    static const std::vector<std::string>& known_probes();
};

} // namespace malibu::compat
