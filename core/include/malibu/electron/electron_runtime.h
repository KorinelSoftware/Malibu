#pragma once
// core/include/malibu/electron/electron_runtime.h
// Malibu Electron Runtime (Tasks 33/35): runs Electron-style apps entirely on
// Malibu — the main process on MalibuJS, each BrowserWindow's renderer on
// MalibuView — with no Node and no Chromium. `require('electron')` yields
// app / BrowserWindow / ipcMain (main) and ipcRenderer (renderer); IPC is
// marshaled across the two realms as JSON (structured-clone analogue).

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "malibu/js/engine.h"
#include "malibu/view/view.h"

namespace malibu::electron {

class ElectronRuntime {
public:
    // Resolves a file path (BrowserWindow.loadFile) to document content.
    using ResourceLoader = std::function<std::optional<std::string>(const std::string& path)>;

    explicit ElectronRuntime(ResourceLoader loader = {});
    ~ElectronRuntime();

    // Evaluates the Electron main script, then fires app 'ready' (resolving
    // app.whenReady()) and drains the main event loop. Returns false on a main
    // script error.
    bool run_main(const std::string& main_script, const std::string& filename = "main.js");

    // Pumps the main process event loop (timers/promises).
    void run_tasks();

    // --- accessors (host / tests) ---
    [[nodiscard]] js::Engine& main_engine() noexcept { return main_; }
    [[nodiscard]] size_t      window_count() const noexcept { return windows_.size(); }
    [[nodiscard]] view::View* window(size_t i) noexcept {
        return i < windows_.size() ? windows_[i]->view.get() : nullptr;
    }
    [[nodiscard]] bool quit_requested() const noexcept { return quit_; }

    // --- IPC plumbing (called by the JS-side natives) ---
    void   renderer_send(size_t win, const std::u16string& channel, const std::vector<std::string>& json_args);
    std::string renderer_invoke(size_t win, const std::u16string& channel, const std::vector<std::string>& json_args);
    void   main_send(size_t win, const std::u16string& channel, const std::vector<std::string>& json_args);
    void   renderer_on(size_t win, const std::u16string& channel, js::runtime::Value handler);

    // Creates a window (BrowserWindow ctor) and returns its index.
    size_t create_window();
    bool   load_file(size_t win, const std::string& path);

private:
    struct Window {
        std::unique_ptr<view::View>                                              view;
        std::unordered_map<std::u16string, std::vector<js::runtime::Value>>       on_handlers;  // ipcRenderer.on
        bool                                                                      ipc_installed = false;
    };

    void install_main_globals();
    void install_renderer_ipc(size_t win);

    // Cross-realm marshaling (JSON).
    static std::string  to_json(js::runtime::Interpreter& src, js::runtime::Value v);
    static js::runtime::Value from_json(js::runtime::Interpreter& dst, const std::string& json);

    js::Engine                                                        main_;
    ResourceLoader                                                    loader_;
    std::vector<std::unique_ptr<Window>>                              windows_;
    std::unordered_map<std::u16string, js::runtime::Value>            ipc_on_;      // ipcMain.on
    std::unordered_map<std::u16string, js::runtime::Value>            ipc_handle_;  // ipcMain.handle
    js::runtime::JSPromise*                                           ready_promise_ = nullptr;
    bool                                                              quit_ = false;
};

} // namespace malibu::electron
