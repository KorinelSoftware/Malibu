#pragma once
// malibu-view/include/malibu/view/view.h
// MalibuView (Task 31): the embeddable engine instance. Orchestrates the whole
// stack — HTML parse -> DOM -> CSS cascade -> JavaScript (DOM via WebCall ABI)
// -> layout -> raster — behind a small embedding surface with eval, request
// interception, bidirectional messaging, and per-document sandboxing.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "malibu/dom/document.h"
#include "malibu/css/style/style_resolver.h"
#include "malibu/layout/layout_engine.h"
#include "malibu/render/renderer.h"
#include "malibu/js/engine.h"
#include "malibu/js/dom_binding.h"
#include "malibu/html/html_parser.h"
#include "malibu/wasm/wasm.h"
#include "malibu/canvas/canvas2d.h"
#include "malibu/gl/gl.h"
#include "malibu/image/png.h"
#include <map>
#include "malibu/storage/storage_engine.h"
#include "malibu/network/fetch_engine.h"
#include "malibu/security/origin.h"
#include "malibu/text/font.h"
#include "malibu/text/text_measurer.h"
#include "malibu/text/glyph_drawer.h"

namespace malibu::view {

// Sandbox capability flags (Req 14.5). All disabled by default in sandbox mode.
enum SandboxFlags : uint32_t {
    SandboxNone          = 0,
    SandboxNoNetwork     = 1u << 0,
    SandboxNoStorage     = 1u << 1,
    SandboxNoNavigation  = 1u << 2,
};

enum class LoadDiagnosticKind : uint8_t {
    Resource,
    Script,
    Unsupported,
};

struct LoadDiagnostic {
    LoadDiagnosticKind kind = LoadDiagnosticKind::Resource;
    std::string url;
    std::string message;
};

class View {
public:
    View();
    ~View();

    // ---- navigation ----
    bool load_html(const std::string& html, const std::string& base_url = "about:blank");
    bool load_file(const std::string& path);
    bool load_url(const std::string& url);
    void reload();
    bool go_back();
    bool go_forward();
    [[nodiscard]] const std::string& current_url() const noexcept { return current_url_; }

    // ---- input / events ----
    // Dispatches a DOM event (e.g. from host window input) to the first element
    // matching `selector`, running capture → target → bubble. Returns false if
    // the event was canceled (preventDefault) or nothing matched.
    bool dispatch_event(const std::string& selector, const std::string& type, bool bubbles = true);

    // ---- scripting ----
    // Evaluates JS in the page realm; returns the JSON-serialized result (or an
    // "error: ..." string on failure).
    std::string eval_js(const std::string& source);

    // ---- rendering ----
    // Renders a `width`x`height` viewport with the page scrolled up by `scroll_y`
    // px (position:fixed stays pinned). scroll_y=0 renders from the top.
    render::Framebuffer render(int width, int height, float scroll_y = 0.0f);
    // Lays out at `width` and returns the full document content height (px), so a
    // host can render the whole page in one tall image / compute scroll extents.
    float page_height(int width);

    // ---- hit testing / interaction (for an interactive host) ----
    // Maps a document-space point to the topmost element node (0 handle if none).
    malibu::NodeHandle node_at(float x, float y);
    // Dispatches a mouse event ("click"/"mousedown"/"mouseup"/"mousemove") at a
    // document-space point. Updates hover/active/focus state. Returns the hit node.
    malibu::NodeHandle dispatch_mouse(float x, float y, const std::string& type, int button = 0);
    // Updates :hover to the element at (x,y); returns true if it changed (host
    // should re-render). Pass a miss (no element) to clear hover.
    bool set_hover(float x, float y);
    // Routes a key / text-input to the focused element (form controls).
    void dispatch_key(const std::string& key, bool is_text);

    // ---- bidirectional messaging ----
    // native -> JS: invokes globalThis.__malibuReceiveMessage(msg) if defined.
    void post_message(const std::string& message);
    // JS -> native: window.malibuNativeMessage(msg) calls this handler.
    void set_message_handler(std::function<void(const std::string&)> handler) {
        message_handler_ = std::move(handler);
    }

    // ---- request interception ----
    // Invoked before the network engine sends a request. Return true (and fill
    // `out`) to satisfy the request locally; false to let it proceed.
    using RequestHandler = std::function<bool(const std::string& url, network::FetchResponse& out)>;
    void set_request_handler(RequestHandler handler) { request_handler_ = std::move(handler); }

    // Diagnostics are reset for every document load. Browsers continue loading
    // after most script/resource failures, so embedders need a separate channel
    // instead of relying on load_html() returning false.
    using DiagnosticHandler = std::function<void(const LoadDiagnostic&)>;
    void set_diagnostic_handler(DiagnosticHandler handler) {
        diagnostic_handler_ = std::move(handler);
    }
    [[nodiscard]] const std::vector<LoadDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

    // ---- WebSocket transport (host-driven; the engine has no TLS of its own) ----
    // When page JS does `new WebSocket(url)`, the view assigns an id and calls
    // this handler so the host can open the real connection. The host then drives
    // the socket via socket_open / socket_message / socket_close, and receives
    // outbound frames (ws.send) through the handler's `send` callback signature.
    // Signature: (id, url, outgoing_or_empty, kind) where kind: 0=open 1=send 2=close.
    using SocketHandler = std::function<void(int id, const std::string& url,
                                             const std::string& data, int kind)>;
    void set_socket_handler(SocketHandler handler) { socket_handler_ = std::move(handler); }
    // native -> JS socket event delivery (host calls these):
    void socket_open(int id);
    void socket_message(int id, const std::string& data);
    void socket_close(int id, int code, const std::string& reason);

    // ---- sandbox ----
    void set_sandbox(uint32_t flags) { sandbox_ = flags; }
    [[nodiscard]] uint32_t sandbox() const noexcept { return sandbox_; }

    // ---- pump the event loop (timers, promises, rAF) ----
    void run_tasks(uint64_t elapsed_ms = 0) { engine_.run_ready_tasks(elapsed_ms); }

    // ---- accessors (for tests / hosts) ----
    dom::Document&        document() { return *doc_; }
    dom::DOMTree&         tree() { return *tree_; }
    js::Engine&           engine() { return engine_; }
    storage::StorageEngine& storage() { return storage_; }

private:
    void do_load(const std::string& html, const std::string& base_url);  // no history push
    void reset_document();
    void apply_styles();
    void run_scripts(const std::vector<malibu::html::ScriptItem>& items);
    void record_diagnostic(LoadDiagnosticKind kind, std::string url,
                           std::string message);
    // Resolves a possibly-relative script/resource URL against the current page.
    std::string resolve_url(const std::u16string& ref) const;
    // Dispatches a window-level event (e.g. DOMContentLoaded / load) to listeners
    // registered via window.addEventListener.
    void fire_window_event(const std::u16string& type);
    void install_view_globals();
    void install_wasm_globals();   // the `WebAssembly` JS API over MalibuWASM
    // Builds (or returns) a rendering context for a <canvas> element. "2d" gives
    // a MalibuCanvas-backed CanvasRenderingContext2D; the bitmap is composited
    // into the page in render().
    js::runtime::Value make_canvas_context(malibu::NodeHandle node, const std::u16string& type);
    js::runtime::Value make_webgl_context(malibu::gl::Context* g);
    void composite_canvases(render::Framebuffer& fb, float scroll_y = 0.0f);
    // Fetches + decodes <img> sources, sizing their boxes (inline style) so they
    // lay out, and registering bitmaps for compositing.
    void load_images(const malibu::html::ParsedDocument& parsed);

    // Engine instance (persists across navigations within this view).
    js::Engine                           engine_;

    // Per-document state (recreated on each navigation).
    std::unique_ptr<dom::Document>       doc_;
    std::unique_ptr<dom::DOMTree>        tree_;
    std::unique_ptr<css::StyleResolver>  resolver_;
    std::unique_ptr<js::DomBinding>      binding_;
    layout::LayoutEngine                 layout_;
    render::Renderer                     renderer_;

    // Real text engine (font shaping/rasterization).
    text::FontSystem                          fonts_;
    std::unique_ptr<text::FreeTypeTextMeasurer> measurer_;
    std::unique_ptr<text::FreeTypeTextDrawer>   drawer_;

    storage::StorageEngine               storage_;
    security::Origin                     origin_;
    std::vector<std::u16string>          pending_stylesheets_;

    // MalibuWASM: decoded modules / live instances, kept alive while JS holds
    // their wrapper objects (the wrappers store an index into these).
    std::vector<std::unique_ptr<malibu::wasm::Module>>   wasm_modules_;
    std::vector<std::unique_ptr<malibu::wasm::Instance>> wasm_instances_;

    // MalibuCanvas: per-<canvas> 2D surfaces, keyed by node handle, composited
    // into the framebuffer in render().
    std::map<uint64_t, std::shared_ptr<malibu::canvas::Canvas2D>> canvases_;
    std::map<uint64_t, std::shared_ptr<malibu::gl::Context>>      gl_contexts_;
    std::map<uint64_t, malibu::image::DecodedImage>              images_;   // decoded <img> bitmaps

    // History (url + the HTML content rendered at each position)
    std::vector<std::string>             history_;
    std::vector<std::string>             history_html_;
    size_t                               history_pos_ = 0;
    std::string                          current_url_ = "about:blank";

    // Dynamic interaction state (drives :hover/:focus/:active + input routing).
    malibu::NodeHandle                   hovered_ = malibu::NodeHandle::null_handle();
    malibu::NodeHandle                   focused_ = malibu::NodeHandle::null_handle();
    // Last viewport size (for @media evaluation during apply_styles()).
    int                                  last_vw_ = 1024, last_vh_ = 768;
    // Restyle+relayout are expensive (apply_styles() re-parses every stylesheet).
    // They run only when content/viewport actually changed — a pure scroll reuses
    // the cached layout and just re-rasterizes at the new offset. Mutators set this.
    bool                                 layout_dirty_ = true;

    std::function<void(const std::string&)> message_handler_;
    RequestHandler                          request_handler_;
    DiagnosticHandler                       diagnostic_handler_;
    std::vector<LoadDiagnostic>              diagnostics_;
    SocketHandler                           socket_handler_;
    // Live WebSocket JS objects keyed by id (host-rooted while connected).
    std::vector<std::pair<int, js::runtime::Value>> sockets_;
    int                                     next_socket_id_ = 1;
    uint32_t                                sandbox_ = SandboxNone;

    void dispatch_socket_event(int id, const char* type, const std::string& data,
                               int code, const std::string& reason);
};

} // namespace malibu::view
