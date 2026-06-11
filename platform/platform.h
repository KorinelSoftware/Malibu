#pragma once
// platform/platform.h
// Pure-virtual Platform API — Malibu Core's only interface to the host OS.
// All implementations live in platform/linux/, platform/windows/, platform/null/.
// NO OS-specific headers may be included here.

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <span>
#include <memory>

namespace malibu {

// ---------------------------------------------------------------------------
// Opaque handle types — platform backends fill these in their own headers.
// Core code only ever holds pointers/IDs; it never dereferences internals.
// ---------------------------------------------------------------------------
using WindowHandle   = uint64_t;  // opaque native window identifier
using ThreadHandle   = uint64_t;  // opaque thread identifier
using VulkanSurface  = void*;     // VkSurfaceKHR (void* avoids Vulkan header dep)

// ---------------------------------------------------------------------------
// Input event types
// ---------------------------------------------------------------------------
enum class KeyAction  : uint8_t { Press, Release, Repeat };
enum class MouseButton: uint8_t { Left, Middle, Right, X1, X2 };
enum class MouseAction: uint8_t { Press, Release, Move, Scroll };

struct KeyEvent {
    uint32_t  key_code;     // platform-neutral virtual key code
    uint32_t  scan_code;    // hardware scan code
    KeyAction action;
    bool      shift, ctrl, alt, super;
};

struct MouseEvent {
    MouseButton button;
    MouseAction action;
    int32_t     x, y;       // position in window-relative pixels
    float       scroll_x, scroll_y;
};

using InputHandler = std::function<void(WindowHandle, const KeyEvent&)>;
using MouseHandler = std::function<void(WindowHandle, const MouseEvent&)>;

// ---------------------------------------------------------------------------
// File I/O result
// ---------------------------------------------------------------------------
struct FileReadResult {
    std::vector<uint8_t> data;
    bool                 success{false};
    std::string          error_message;
};

struct FileWriteResult {
    bool        success{false};
    std::string error_message;
};

// ---------------------------------------------------------------------------
// Network and font system are opaque pointers; their concrete types live in
// the platform backends and the network/font subsystems respectively.
// ---------------------------------------------------------------------------
class INetworkStack;
class IFontSystem;

// ---------------------------------------------------------------------------
// Render error codes
// ---------------------------------------------------------------------------
enum class RenderError : uint32_t {
    DeviceLost,
    SwapchainLost,
    OutOfMemory,
    Unknown,
};

// ---------------------------------------------------------------------------
// Platform — pure-virtual interface.
//
// PlatformRegistry::register_backend(Platform*) validates that all methods
// are overridden (i.e., the vtable is fully populated) before setting the
// global backend. Partial implementations are rejected.
// ---------------------------------------------------------------------------
class Platform {
public:
    virtual ~Platform() = default;

    // --- Window management ------------------------------------------------
    /// Create a native window. Returns WindowHandle(0) on failure.
    virtual WindowHandle create_window(
        std::string_view title,
        int32_t          width,
        int32_t          height) = 0;

    /// Destroy a window previously created with create_window.
    virtual void destroy_window(WindowHandle window) = 0;

    // --- Vulkan surface ---------------------------------------------------
    /// Create a Vulkan-compatible surface for the given window.
    /// Returns nullptr on failure.
    virtual VulkanSurface create_vulkan_surface(WindowHandle window) = 0;

    // --- Input ------------------------------------------------------------
    /// Register keyboard event handler for a window.
    virtual void set_key_handler(
        WindowHandle        window,
        InputHandler        handler) = 0;

    /// Register mouse event handler for a window.
    virtual void set_mouse_handler(
        WindowHandle        window,
        MouseHandler        handler) = 0;

    // --- File system ------------------------------------------------------
    /// Synchronous file read. Reads entire file into memory.
    virtual FileReadResult read_file(std::string_view path) = 0;

    /// Synchronous file write. Overwrites or creates the file.
    virtual FileWriteResult write_file(
        std::string_view        path,
        std::span<const uint8_t> data) = 0;

    // --- Threading --------------------------------------------------------
    /// Spawn a new OS thread executing `task`. Returns opaque ThreadHandle.
    virtual ThreadHandle spawn_thread(std::function<void()> task) = 0;

    /// Block the calling thread until the thread identified by `handle` exits.
    virtual void join_thread(ThreadHandle handle) = 0;

    // --- Clock ------------------------------------------------------------
    /// Monotonically increasing wall-clock time in milliseconds.
    /// The epoch is implementation-defined; only deltas are meaningful.
    virtual uint64_t monotonic_clock_ms() = 0;

    // --- Subsystem accessors ---------------------------------------------
    /// Returns the platform's network stack implementation (never null after
    /// a successful register_backend call).
    virtual INetworkStack* network_stack() = 0;

    /// Returns the platform's font system implementation (never null after
    /// a successful register_backend call).
    virtual IFontSystem* font_system() = 0;

    // --- Error reporting --------------------------------------------------
    /// Called by the Vulkan renderer when a non-recoverable render error
    /// occurs. The platform may display a dialog, log, or terminate.
    virtual void on_render_error(RenderError error) = 0;
};

// ---------------------------------------------------------------------------
// PlatformRegistry — singleton that holds the active platform backend.
//
// register_backend() validates completeness (all pure-virtual methods must
// be overridden) before accepting the backend. Returns false and logs
// unimplemented method names on failure.
//
// The implementation lives in platform/null/platform_registry.cpp so that
// the null backend can be linked independently of any OS backend.
// ---------------------------------------------------------------------------
class PlatformRegistry {
public:
    /// Register `backend` as the active platform implementation.
    /// Returns true on success; false if the backend is incomplete.
    static bool register_backend(Platform* backend);

    /// Returns the active backend. Asserts (aborts) if none is registered.
    static Platform& get();

    /// Returns true if a backend has been successfully registered.
    static bool is_registered() noexcept;

private:
    static Platform* s_backend_;
};

} // namespace malibu
