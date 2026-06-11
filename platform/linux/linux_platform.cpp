// platform/linux/linux_platform.cpp
// Linux Platform Backend — POSIX file/clock/thread + display detection.

#include "linux_platform.h"
#include "malibu/diagnostics/diagnostic_log.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace malibu {

LinuxPlatform::LinuxPlatform() {
    // Prefer Wayland, fall back to X11, else headless.
    if (const char* wl = std::getenv("WAYLAND_DISPLAY"); wl && *wl)
        display_server_ = DisplayServer::Wayland;
    else if (const char* x = std::getenv("DISPLAY"); x && *x)
        display_server_ = DisplayServer::X11;
    else
        display_server_ = DisplayServer::None;
}

WindowHandle LinuxPlatform::create_window(std::string_view title, int32_t width, int32_t height) {
    if (display_server_ == DisplayServer::None) {
        MALIBU_LOG(WARNING, "platform.linux",
                   "create_window requested but no Wayland/X11 display is available (headless)");
        return 0;
    }
    // A real desktop session would create a wl_surface / xcb_window here. That
    // path is gated on a live display and is not exercised in headless CI.
    (void)title; (void)width; (void)height;
    MALIBU_LOG(INFO, "platform.linux",
               std::string("create_window on ") +
                   (display_server_ == DisplayServer::Wayland ? "Wayland" : "X11") +
                   " (surface creation pending live-display integration)");
    return 0;
}

void LinuxPlatform::destroy_window(WindowHandle window) {
    std::lock_guard lock(handlers_mu_);
    key_handlers_.erase(window);
    mouse_handlers_.erase(window);
}

VulkanSurface LinuxPlatform::create_vulkan_surface(WindowHandle) {
    // Requires a live window (vkCreateWaylandSurfaceKHR / vkCreateXcbSurfaceKHR).
    return nullptr;
}

void LinuxPlatform::set_key_handler(WindowHandle window, InputHandler handler) {
    std::lock_guard lock(handlers_mu_);
    key_handlers_[window] = std::move(handler);
}
void LinuxPlatform::set_mouse_handler(WindowHandle window, MouseHandler handler) {
    std::lock_guard lock(handlers_mu_);
    mouse_handlers_[window] = std::move(handler);
}

FileReadResult LinuxPlatform::read_file(std::string_view path) {
    std::ifstream f(std::string(path), std::ios::binary);
    if (!f) return {{}, false, "cannot open: " + std::string(path)};
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return {std::move(data), true, {}};
}

FileWriteResult LinuxPlatform::write_file(std::string_view path, std::span<const uint8_t> data) {
    std::ofstream f(std::string(path), std::ios::binary | std::ios::trunc);
    if (!f) return {false, "cannot open for write: " + std::string(path)};
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return {f.good(), f.good() ? std::string() : "write failed"};
}

ThreadHandle LinuxPlatform::spawn_thread(std::function<void()> task) {
    auto* t = new std::thread(std::move(task));
    return reinterpret_cast<ThreadHandle>(t);
}
void LinuxPlatform::join_thread(ThreadHandle handle) {
    auto* t = reinterpret_cast<std::thread*>(handle);
    if (t) { if (t->joinable()) t->join(); delete t; }
}

uint64_t LinuxPlatform::monotonic_clock_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void LinuxPlatform::on_render_error(RenderError error) {
    MALIBU_LOG(ERROR, "platform.linux",
               "render error: " + std::to_string(static_cast<uint32_t>(error)));
}

} // namespace malibu
