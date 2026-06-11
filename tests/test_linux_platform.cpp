// tests/test_linux_platform.cpp
// Task 29: Linux Platform Backend — POSIX file I/O, monotonic clock, threads,
// display detection, and registry acceptance.

#include <gtest/gtest.h>
#include "linux_platform.h"

#include <atomic>
#include <cstdio>
#include <string>

using malibu::LinuxPlatform;

TEST(LinuxPlatform, FileReadWriteRoundTrip) {
    LinuxPlatform p;
    const std::string path = "test_linux_platform_file.bin";
    std::string content = "Malibu platform backend \x01\x02\x03 bytes";
    std::vector<uint8_t> data(content.begin(), content.end());

    auto w = p.write_file(path, std::span<const uint8_t>(data));
    ASSERT_TRUE(w.success) << w.error_message;

    auto r = p.read_file(path);
    ASSERT_TRUE(r.success) << r.error_message;
    EXPECT_EQ(std::string(r.data.begin(), r.data.end()), content);

    std::remove(path.c_str());
}

TEST(LinuxPlatform, ReadMissingFileFails) {
    LinuxPlatform p;
    auto r = p.read_file("/no/such/malibu/file");
    EXPECT_FALSE(r.success);
}

TEST(LinuxPlatform, MonotonicClockNonDecreasing) {
    LinuxPlatform p;
    uint64_t a = p.monotonic_clock_ms();
    uint64_t b = p.monotonic_clock_ms();
    EXPECT_GE(b, a);
}

TEST(LinuxPlatform, ThreadRunsAndJoins) {
    LinuxPlatform p;
    std::atomic<int> counter{0};
    auto h = p.spawn_thread([&] { counter.fetch_add(21); });
    p.join_thread(h);
    EXPECT_EQ(counter.load(), 21);
}

TEST(LinuxPlatform, DisplayDetectionReturnsAValue) {
    LinuxPlatform p;
    // Whatever the environment, detection must classify it without crashing.
    auto ds = p.display_server();
    EXPECT_TRUE(ds == LinuxPlatform::DisplayServer::None ||
                ds == LinuxPlatform::DisplayServer::Wayland ||
                ds == LinuxPlatform::DisplayServer::X11);
    // Headless create_window returns 0 rather than crashing.
    if (!p.has_display()) { EXPECT_EQ(p.create_window("t", 800, 600), 0u); }
}

TEST(LinuxPlatform, RegistersAsBackend) {
    LinuxPlatform p;
    EXPECT_TRUE(malibu::PlatformRegistry::register_backend(&p));
    EXPECT_TRUE(malibu::PlatformRegistry::is_registered());
}
