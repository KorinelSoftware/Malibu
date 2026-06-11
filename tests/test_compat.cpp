// tests/test_compat.cpp
// Task 20: Static Pre-Profiler probe detection + Expectation Layer leveling.

#include <gtest/gtest.h>
#include "malibu/compat/static_pre_profiler.h"
#include "malibu/compat/expectation_layer.h"

#include <algorithm>

using namespace malibu::compat;

namespace {
bool has(const ProbeReport& r, const std::string& id) {
    return std::find(r.detected_probes.begin(), r.detected_probes.end(), id) !=
           r.detected_probes.end();
}
}  // namespace

TEST(StaticPreProfiler, DetectsDotNotation) {
    StaticPreProfiler p;
    auto r = p.scan(u"if (window.chrome) { console.log(navigator.userAgent); }");
    EXPECT_FALSE(r.tokenization_failed);
    EXPECT_TRUE(has(r, "window.chrome"));
    EXPECT_TRUE(has(r, "navigator.userAgent"));
}

TEST(StaticPreProfiler, DetectsBracketNotation) {
    StaticPreProfiler p;
    auto r = p.scan(uR"(var x = navigator["vendor"]; var y = window['opr'];)");
    EXPECT_TRUE(has(r, "navigator.vendor"));
    EXPECT_TRUE(has(r, "window.opr"));
}

TEST(StaticPreProfiler, DetectsDestructuring) {
    StaticPreProfiler p;
    auto r = p.scan(u"const { userAgent, vendor } = navigator;");
    EXPECT_TRUE(has(r, "navigator.userAgent"));
    EXPECT_TRUE(has(r, "navigator.vendor"));
}

TEST(StaticPreProfiler, NoProbesGivesEmptyReport) {
    StaticPreProfiler p;
    auto r = p.scan(u"function add(a, b) { return a + b; }");
    EXPECT_FALSE(r.tokenization_failed);
    EXPECT_TRUE(r.detected_probes.empty());
}

TEST(StaticPreProfiler, BinarySourceFlaggedAndNoProbes) {
    StaticPreProfiler p;
    std::u16string bin;
    for (int i = 0; i < 50; ++i) bin.push_back(static_cast<char16_t>(i % 7 + 1));  // control bytes
    auto r = p.scan(bin);
    EXPECT_TRUE(r.tokenization_failed);
    EXPECT_TRUE(r.detected_probes.empty());
}

TEST(StaticPreProfiler, DoesNotFalsePositiveOnUnknownProperty) {
    StaticPreProfiler p;
    auto r = p.scan(u"window.location; navigator.language; document.title;");
    EXPECT_TRUE(r.detected_probes.empty());
}

TEST(ExpectationLayer, StaticProfileConfiguresDetectedProbes) {
    StaticPreProfiler sp;
    ProbeReport rep = sp.scan(u"window.chrome; navigator.userAgent;");
    ExpectationLayer layer;
    layer.configure_from_probe_report(rep);

    auto chrome = layer.query_level(CompatLevel::StaticPreProfile, "window.chrome");
    EXPECT_TRUE(chrome.active);
    EXPECT_EQ(chrome.value, "{}");
    auto ua = layer.query_level(CompatLevel::StaticPreProfile, "navigator.userAgent");
    EXPECT_TRUE(ua.active);
    EXPECT_NE(ua.value.find("Chrome"), std::string::npos);
}

TEST(ExpectationLayer, UndetectedProbeReturnsUndefined) {
    ExpectationLayer layer;
    layer.note_first_execute(1000);
    // navigator.userAgent never detected/observed yet, but it IS a known probe;
    // accessing it now counts as a runtime observation, so it becomes active.
    // A truly unknown probe must stay undefined.
    auto r = layer.respond("window.somethingWeNeverHeardOf", 1010);
    EXPECT_FALSE(r.active);
    EXPECT_TRUE(r.value.empty());
}

TEST(ExpectationLayer, LevelSelectionByElapsedTime) {
    ExpectationLayer layer;
    layer.note_first_execute(1000);
    layer.respond("navigator.userAgent", 1100);  // +100ms
    EXPECT_EQ(layer.current_level(), CompatLevel::EarlyRuntimeProfile);
    layer.respond("navigator.userAgent", 1600);  // +600ms
    EXPECT_EQ(layer.current_level(), CompatLevel::AdaptiveRuntime);
}

TEST(ExpectationLayer, DoesNotImpersonateNonChromeBrowsers) {
    ExpectationLayer layer;
    layer.note_first_execute(0);
    // Even though these are known probes, Malibu is not Opera/Firefox/IE.
    EXPECT_FALSE(layer.respond("window.opr", 10).active);
    EXPECT_FALSE(layer.respond("window.InstallTrigger", 10).active);
    EXPECT_FALSE(layer.respond("document.documentMode", 10).active);
    // But it does expose a Chrome-compatible surface when probed.
    EXPECT_TRUE(layer.respond("navigator.userAgent", 10).active);
}
