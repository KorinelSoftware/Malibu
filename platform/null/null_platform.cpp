// platform/null/null_platform.cpp
// Null Platform Backend - in-memory FS, deterministic clock, no-op window/render.

#include "platform.h"
#include <unordered_map>
#include <mutex>
#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include <cstring>

namespace malibu {

class NullPlatform : public Platform {
public:
    // In-memory filesystem
    FileReadResult read_file(std::string_view path) override {
        std::lock_guard lock(fs_mu_);
        auto it = fs_.find(std::string(path));
        if (it != fs_.end()) {
            return {it->second, true, {}};
        }
        return {{}, false, "Not found: " + std::string(path)};
    }

    FileWriteResult write_file(std::string_view path, std::span<const uint8_t> data) override {
        std::lock_guard lock(fs_mu_);
        fs_[std::string(path)] = std::vector<uint8_t>(data.begin(), data.end());
        return {true, {}};
    }

    // Window management - no-op
    WindowHandle create_window(std::string_view, int32_t, int32_t) override {
        return ++window_counter_;
    }

    void destroy_window(WindowHandle) override {}

    // Vulkan surface - returns null (no actual rendering)
    VulkanSurface create_vulkan_surface(WindowHandle) override {
        return nullptr;
    }

    // Input handlers - no-op
    void set_key_handler(WindowHandle, InputHandler) override {}
    void set_mouse_handler(WindowHandle, MouseHandler) override {}

    // Threading
    ThreadHandle spawn_thread(std::function<void()> task) override {
        auto thread = std::make_unique<std::thread>(std::move(task));
        ThreadHandle handle = reinterpret_cast<ThreadHandle>(thread.release());
        return handle;
    }

    void join_thread(ThreadHandle handle) override {
        auto thread = reinterpret_cast<std::thread*>(handle);
        if (thread) {
            thread->join();
            delete thread;
        }
    }

    // Clock - deterministic, increments by ~16ms (60fps) per call
    uint64_t monotonic_clock_ms() override {
        std::lock_guard lock(clock_mu_);
        clock_ms_ += 16;
        return clock_ms_;
    }

    // Subsystem accessors - return null for null backend
    INetworkStack* network_stack() override { return nullptr; }
    IFontSystem* font_system() override { return nullptr; }

    // Error reporting - no-op
    void on_render_error(RenderError) override {}

    // Test helper: inject file into virtual filesystem
    void inject_file(std::string_view path, std::span<const uint8_t> data) {
        std::lock_guard lock(fs_mu_);
        fs_[std::string(path)] = std::vector<uint8_t>(data.begin(), data.end());
    }

    // Test helper: set clock manually
    void set_clock(uint64_t ms) {
        std::lock_guard lock(clock_mu_);
        clock_ms_ = ms;
    }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> fs_;
    std::mutex fs_mu_;

    uint64_t clock_ms_ = 0;
    std::mutex clock_mu_;

    WindowHandle window_counter_ = 0;
};

// Global null platform instance
static NullPlatform g_null_platform;

// PlatformRegistry implementation
Platform* PlatformRegistry::s_backend_ = nullptr;

bool PlatformRegistry::register_backend(Platform* backend) {
    if (!backend) return false;

    // Validate that backend implements all pure virtual methods
    // This is a runtime smoke test - if any method is pure virtual,
    // the call will abort or throw.
    try {
        backend->create_window("", 0, 0);
        backend->destroy_window(0);
        backend->create_vulkan_surface(0);
        backend->set_key_handler(0, nullptr);
        backend->set_mouse_handler(0, nullptr);
        backend->read_file("");
        backend->write_file("", {});
        ThreadHandle smoke_thread = backend->spawn_thread([]{});
        backend->join_thread(smoke_thread);
        backend->monotonic_clock_ms();
        backend->network_stack();
        backend->font_system();
        backend->on_render_error(RenderError::Unknown);
    } catch (...) {
        return false;
    }

    s_backend_ = backend;
    return true;
}

Platform& PlatformRegistry::get() {
    if (!s_backend_) {
        // Auto-register null backend if nothing registered
        register_backend(&g_null_platform);
    }
    return *s_backend_;
}

bool PlatformRegistry::is_registered() noexcept {
    return s_backend_ != nullptr;
}

} // namespace malibu