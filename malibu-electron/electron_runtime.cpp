// malibu-electron/electron_runtime.cpp
// Runs Electron-style apps on Malibu: main process on MalibuJS, each
// BrowserWindow renderer on MalibuView, IPC marshaled across realms as JSON.

#include "malibu/electron/electron_runtime.h"

namespace malibu::electron {

using js::runtime::Value;
using js::runtime::Interpreter;

namespace {
std::string narrow(const std::u16string& s) {
    std::string r; r.reserve(s.size());
    for (char16_t c : s) r.push_back(static_cast<char>(c & 0xFF));
    return r;
}
}  // namespace

ElectronRuntime::ElectronRuntime(ResourceLoader loader) : loader_(std::move(loader)) {
    install_main_globals();
}
ElectronRuntime::~ElectronRuntime() = default;

std::string ElectronRuntime::to_json(Interpreter& src, Value v) {
    return narrow(src.json_stringify(v));
}

Value ElectronRuntime::from_json(Interpreter& dst, const std::string& json) {
    if (json.empty() || json == "undefined") return Value::make_undefined();
    Value* j = dst.global()->find(u"JSON");
    if (!j) return Value::make_undefined();
    Value parse = dst.get_prop_public(*j, u"parse");
    if (!dst.is_callable(parse)) return Value::make_undefined();
    std::vector<Value> args{dst.str(json)};
    try { return dst.call(parse, Value::make_undefined(), args); }
    catch (js::runtime::ThrowSignal&) { return Value::make_undefined(); }
}

// ---------------------------------------------------------------------------
// Main-process globals: require('electron') -> { app, BrowserWindow, ipcMain }
// ---------------------------------------------------------------------------
void ElectronRuntime::install_main_globals() {
    Interpreter& in = main_.interpreter();
    ElectronRuntime* self = this;

    auto* electron = in.new_object();
    Value electronv = Value::make_heap_ptr(electron);

    // ---- app ----
    auto* app = in.new_object();
    Value appv = Value::make_heap_ptr(app);
    in.set_prop_public(appv, u"whenReady", Value::make_heap_ptr(in.new_native(u"whenReady",
        [self](Interpreter& i, Value, std::vector<Value>&) -> Value {
            if (!self->ready_promise_) {
                self->ready_promise_ = i.new_promise();
                i.add_host_root(Value::make_heap_ptr(self->ready_promise_));
            }
            return Value::make_heap_ptr(self->ready_promise_);
        })));
    in.set_prop_public(appv, u"quit", Value::make_heap_ptr(in.new_native(u"quit",
        [self](Interpreter&, Value, std::vector<Value>&) -> Value { self->quit_ = true; return Value::make_undefined(); })));
    in.set_prop_public(appv, u"on", Value::make_heap_ptr(in.new_native(u"on",
        [](Interpreter&, Value, std::vector<Value>&) -> Value { return Value::make_undefined(); })));
    in.set_prop_public(appv, u"getName", Value::make_heap_ptr(in.new_native(u"getName",
        [](Interpreter& i, Value, std::vector<Value>&) -> Value { return i.str("MalibuApp"); })));
    in.set_prop_public(appv, u"getVersion", Value::make_heap_ptr(in.new_native(u"getVersion",
        [](Interpreter& i, Value, std::vector<Value>&) -> Value { return i.str("1.0.0"); })));
    in.set_prop_public(electronv, u"app", appv);

    // ---- BrowserWindow ----
    auto* bw = in.new_native(u"BrowserWindow",
        [self](Interpreter& i, Value, std::vector<Value>&) -> Value {
            size_t idx = self->create_window();
            auto* win = i.new_object();
            Value winv = Value::make_heap_ptr(win);
            i.set_prop_public(winv, u"id", Value::make_int32(static_cast<int32_t>(idx)));
            i.set_prop_public(winv, u"loadFile", Value::make_heap_ptr(i.new_native(u"loadFile",
                [self, idx](Interpreter& i2, Value, std::vector<Value>& a) -> Value {
                    bool ok = !a.empty() && self->load_file(idx, narrow(i2.to_string(a[0])));
                    return Value::make_bool(ok);
                })));
            i.set_prop_public(winv, u"loadURL", Value::make_heap_ptr(i.new_native(u"loadURL",
                [self, idx](Interpreter& i2, Value, std::vector<Value>& a) -> Value {
                    bool ok = !a.empty() && self->load_file(idx, narrow(i2.to_string(a[0])));
                    return Value::make_bool(ok);
                })));
            // webContents.send(channel, ...args)  → renderer
            auto* wc = i.new_object();
            Value wcv = Value::make_heap_ptr(wc);
            i.set_prop_public(wcv, u"send", Value::make_heap_ptr(i.new_native(u"send",
                [self, idx](Interpreter& i2, Value, std::vector<Value>& a) -> Value {
                    if (!a.empty()) {
                        std::vector<std::string> args;
                        for (size_t k = 1; k < a.size(); ++k) args.push_back(to_json(i2, a[k]));
                        self->main_send(idx, i2.to_string(a[0]), args);
                    }
                    return Value::make_undefined();
                })));
            i.set_prop_public(winv, u"webContents", wcv);
            i.set_prop_public(winv, u"on", Value::make_heap_ptr(i.new_native(u"on",
                [](Interpreter&, Value, std::vector<Value>&) -> Value { return Value::make_undefined(); })));
            i.set_prop_public(winv, u"close", Value::make_heap_ptr(i.new_native(u"close",
                [](Interpreter&, Value, std::vector<Value>&) -> Value { return Value::make_undefined(); })));
            return winv;
        });
    in.set_prop_public(electronv, u"BrowserWindow", Value::make_heap_ptr(bw));

    // ---- ipcMain ----
    auto* ipc_main = in.new_object();
    Value imv = Value::make_heap_ptr(ipc_main);
    in.set_prop_public(imv, u"on", Value::make_heap_ptr(in.new_native(u"on",
        [self](Interpreter& i, Value, std::vector<Value>& a) -> Value {
            if (a.size() >= 2 && i.is_callable(a[1])) { self->ipc_on_[i.to_string(a[0])] = a[1]; i.add_host_root(a[1]); }
            return Value::make_undefined();
        })));
    in.set_prop_public(imv, u"handle", Value::make_heap_ptr(in.new_native(u"handle",
        [self](Interpreter& i, Value, std::vector<Value>& a) -> Value {
            if (a.size() >= 2 && i.is_callable(a[1])) { self->ipc_handle_[i.to_string(a[0])] = a[1]; i.add_host_root(a[1]); }
            return Value::make_undefined();
        })));
    in.set_prop_public(electronv, u"ipcMain", imv);

    // ---- require ----
    auto* require = in.new_native(u"require",
        [electronv](Interpreter& i, Value, std::vector<Value>& a) -> Value {
            if (!a.empty() && narrow(i.to_string(a[0])) == "electron") return electronv;
            return Value::make_heap_ptr(i.new_object());  // best-effort: unknown module
        });
    in.global()->define(u"require", Value::make_heap_ptr(require));
    in.add_host_root(electronv);  // keeps app/BrowserWindow/ipcMain alive
}

bool ElectronRuntime::run_main(const std::string& main_script, const std::string& filename) {
    auto r = main_.evaluate(main_script, filename);
    if (!r.ok) return false;
    // Fire app 'ready' → resolve app.whenReady(), then run the event loop so the
    // ready handler (which creates windows) executes.
    if (ready_promise_) main_.interpreter().resolve_promise(ready_promise_, Value::make_undefined());
    main_.run_event_loop();
    return true;
}

void ElectronRuntime::run_tasks() { main_.run_event_loop(); }

size_t ElectronRuntime::create_window() {
    auto w = std::make_unique<Window>();
    w->view = std::make_unique<view::View>();
    windows_.push_back(std::move(w));
    return windows_.size() - 1;
}

bool ElectronRuntime::load_file(size_t win, const std::string& path) {
    if (win >= windows_.size()) return false;
    std::optional<std::string> content = loader_ ? loader_(path) : std::nullopt;
    if (!content) return false;
    install_renderer_ipc(win);                       // before the renderer scripts run
    return windows_[win]->view->load_html(*content, "file://" + path);
}

// ---------------------------------------------------------------------------
// Renderer globals: require('electron') -> { ipcRenderer }
// ---------------------------------------------------------------------------
void ElectronRuntime::install_renderer_ipc(size_t win) {
    Window* w = windows_[win].get();
    if (w->ipc_installed) return;
    w->ipc_installed = true;

    Interpreter& in = w->view->engine().interpreter();
    ElectronRuntime* self = this;

    auto* ipcr = in.new_object();
    Value ipcrv = Value::make_heap_ptr(ipcr);
    in.set_prop_public(ipcrv, u"send", Value::make_heap_ptr(in.new_native(u"send",
        [self, win](Interpreter& i, Value, std::vector<Value>& a) -> Value {
            if (!a.empty()) {
                std::vector<std::string> args;
                for (size_t k = 1; k < a.size(); ++k) args.push_back(to_json(i, a[k]));
                self->renderer_send(win, i.to_string(a[0]), args);
            }
            return Value::make_undefined();
        })));
    in.set_prop_public(ipcrv, u"invoke", Value::make_heap_ptr(in.new_native(u"invoke",
        [self, win](Interpreter& i, Value, std::vector<Value>& a) -> Value {
            auto* p = i.new_promise();
            if (!a.empty()) {
                std::vector<std::string> args;
                for (size_t k = 1; k < a.size(); ++k) args.push_back(to_json(i, a[k]));
                std::string result = self->renderer_invoke(win, i.to_string(a[0]), args);
                i.resolve_promise(p, from_json(i, result));
            } else {
                i.resolve_promise(p, Value::make_undefined());
            }
            return Value::make_heap_ptr(p);
        })));
    in.set_prop_public(ipcrv, u"on", Value::make_heap_ptr(in.new_native(u"on",
        [self, win](Interpreter& i, Value, std::vector<Value>& a) -> Value {
            if (a.size() >= 2 && i.is_callable(a[1])) self->renderer_on(win, i.to_string(a[0]), a[1]);
            return Value::make_undefined();
        })));
    in.global()->define(u"ipcRenderer", ipcrv);
    in.add_host_root(ipcrv);

    auto* require = in.new_native(u"require",
        [ipcrv](Interpreter& i, Value, std::vector<Value>& a) -> Value {
            if (!a.empty() && narrow(i.to_string(a[0])) == "electron") {
                auto* m = i.new_object();
                Value mv = Value::make_heap_ptr(m);
                i.set_prop_public(mv, u"ipcRenderer", ipcrv);
                return mv;
            }
            return Value::make_heap_ptr(i.new_object());
        });
    in.global()->define(u"require", Value::make_heap_ptr(require));
}

// ---------------------------------------------------------------------------
// IPC routing
// ---------------------------------------------------------------------------
void ElectronRuntime::renderer_send(size_t, const std::u16string& channel,
                                    const std::vector<std::string>& json_args) {
    auto it = ipc_on_.find(channel);
    if (it == ipc_on_.end()) return;
    Interpreter& mi = main_.interpreter();
    std::vector<Value> args;
    args.push_back(Value::make_heap_ptr(mi.new_object()));  // event {}
    for (auto& j : json_args) args.push_back(from_json(mi, j));
    try { mi.call(it->second, Value::make_undefined(), args); }
    catch (js::runtime::ThrowSignal&) {}
    mi.run_microtasks();
}

std::string ElectronRuntime::renderer_invoke(size_t, const std::u16string& channel,
                                             const std::vector<std::string>& json_args) {
    auto it = ipc_handle_.find(channel);
    if (it == ipc_handle_.end()) return "null";
    Interpreter& mi = main_.interpreter();
    std::vector<Value> args;
    args.push_back(Value::make_heap_ptr(mi.new_object()));
    for (auto& j : json_args) args.push_back(from_json(mi, j));
    Value result;
    try {
        result = mi.call(it->second, Value::make_undefined(), args);
        result = mi.await_settled(result);  // handler may be async
    } catch (js::runtime::ThrowSignal&) { return "null"; }
    return to_json(mi, result);
}

void ElectronRuntime::main_send(size_t win, const std::u16string& channel,
                                const std::vector<std::string>& json_args) {
    if (win >= windows_.size()) return;
    auto hit = windows_[win]->on_handlers.find(channel);
    if (hit == windows_[win]->on_handlers.end()) return;
    Interpreter& ri = windows_[win]->view->engine().interpreter();
    std::vector<Value> handlers = hit->second;  // copy
    for (Value h : handlers) {
        std::vector<Value> args;
        args.push_back(Value::make_heap_ptr(ri.new_object()));
        for (auto& j : json_args) args.push_back(from_json(ri, j));
        try { ri.call(h, Value::make_undefined(), args); }
        catch (js::runtime::ThrowSignal&) {}
    }
    ri.run_microtasks();
}

void ElectronRuntime::renderer_on(size_t win, const std::u16string& channel, Value handler) {
    if (win >= windows_.size()) return;
    windows_[win]->view->engine().interpreter().add_host_root(handler);
    windows_[win]->on_handlers[channel].push_back(handler);
}

} // namespace malibu::electron
