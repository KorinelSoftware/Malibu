#pragma once
// platform/linux/linux_platform.h
// Linux Platform Backend (Task 29). Implements the OS facilities Malibu needs:
// POSIX file I/O, a monotonic clock, native threads, and (on a desktop session)
// Wayland/X11 windowing + a Vulkan surface. OS-specific code is allowed here.

#include "platform.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace malibu {

class LinuxPlatform : public Platform {
public:
    LinuxPlatform();

    // Window management (Wayland preferred, X11 fallback; requires a display).
    WindowHandle  create_window(std::string_view title, int32_t width, int32_t height) override;
    void          destroy_window(WindowHandle window) override;
    VulkanSurface create_vulkan_surface(WindowHandle window) override;

    // Input
    void set_key_handler(WindowHandle window, InputHandler handler) override;
    void set_mouse_handler(WindowHandle window, MouseHandler handler) override;

    // File system (POSIX)
    FileReadResult  read_file(std::string_view path) override;
    FileWriteResult write_file(std::string_view path, std::span<const uint8_t> data) override;

    // Threading (pthreads via std::thread)
    ThreadHandle spawn_thread(std::function<void()> task) override;
    void         join_thread(ThreadHandle handle) override;

    // Clock (CLOCK_MONOTONIC via steady_clock)
    uint64_t monotonic_clock_ms() override;

    // Subsystem accessors
    INetworkStack* network_stack() override { return nullptr; }
    IFontSystem*   font_system() override { return nullptr; }

    void on_render_error(RenderError error) override;

    // Which display server was detected at construction.
    enum class DisplayServer { None, Wayland, X11 };
    [[nodiscard]] DisplayServer display_server() const noexcept { return display_server_; }
    [[nodiscard]] bool has_display() const noexcept { return display_server_ != DisplayServer::None; }

private:
    DisplayServer                                   display_server_ = DisplayServer::None;
    std::atomic<WindowHandle>                        next_window_{1};
    std::mutex                                       handlers_mu_;
    std::unordered_map<WindowHandle, InputHandler>   key_handlers_;
    std::unordered_map<WindowHandle, MouseHandler>   mouse_handlers_;
};

} // namespace malibu
