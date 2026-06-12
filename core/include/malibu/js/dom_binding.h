#pragma once
// core/include/malibu/js/dom_binding.h
// Binds the live DOM into the MalibuJS runtime. DOM node values route property
// reads/writes and method calls through the WebCall ABI (guards + deopt), which
// is the core "DOM as a native part of the VM" thesis of Malibu, driven here by
// real JavaScript source.

#include "malibu/js/runtime/interpreter.h"
#include "malibu/webcall/webcall_dispatch.h"
#include "malibu/js/bytecode/bytecode.h"
#include "malibu/types.h"

#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace malibu::dom { class DOMTree; }

namespace malibu::js {

class DomBinding {
public:
    DomBinding(runtime::Interpreter& interp, malibu::dom::DOMTree& tree,
               malibu::NodeHandle document_root);
    ~DomBinding();

    // Installs DOM property/method hooks and defines the global `document`.
    void install();

    // Host-provided factory for `element.getContext(type)`. Lets the View back
    // <canvas> with MalibuCanvas/MalibuGL without coupling the DOM layer to them.
    using ContextProvider = std::function<runtime::Value(malibu::NodeHandle, const std::u16string&)>;
    void set_context_provider(ContextProvider p) { context_provider_ = std::move(p); }

    // Host-provided layout-box geometry for getBoundingClientRect/offset*/client*.
    // Fills x,y,w,h (CSS px, document space) and returns true if laid out.
    using RectProvider = std::function<bool(malibu::NodeHandle, float&, float&, float&, float&)>;
    void set_rect_provider(RectProvider p) { rect_provider_ = std::move(p); }

    // Notifies the embedding view after DOM mutations. The view uses this to
    // invalidate rendering and prepare connected resource elements such as
    // dynamically-created scripts and stylesheets.
    using MutationHandler = std::function<void(malibu::NodeHandle)>;
    void set_mutation_handler(MutationHandler h) { mutation_handler_ = std::move(h); }

    using CookieGetter = std::function<std::u16string()>;
    using CookieSetter = std::function<void(const std::u16string&)>;
    void set_cookie_handlers(CookieGetter getter, CookieSetter setter) {
        cookie_getter_ = std::move(getter);
        cookie_setter_ = std::move(setter);
    }

    // Invoked after an uncanceled form submission event. The embedding view
    // owns URL resolution, form encoding, network policy, and navigation.
    using SubmitHandler = std::function<void(malibu::NodeHandle)>;
    void set_submit_handler(SubmitHandler handler) {
        submit_handler_ = std::move(handler);
    }

    // Dispatches a DOM event to `target` running capture → target → bubble
    // phases (WHATWG DOM). Returns false iff the event was canceled
    // (preventDefault on a cancelable event). Host-initiated input (a window
    // click/keypress) calls this to drive page interactivity.
    bool dispatch_event(malibu::NodeHandle target, const std::u16string& type,
                        bool bubbles = true, bool cancelable = true);

    // Resolves a JS DOM-node wrapper value back to its NodeHandle (null if it
    // isn't a node). Used by host globals like getComputedStyle.
    [[nodiscard]] malibu::NodeHandle node_of(runtime::Value v) const;

private:
    runtime::Value invoke(uint32_t webcall_id, malibu::NodeHandle target,
                          const malibu::webcall::WebCallArgs& args);
    runtime::Value box_to_value(const malibu::webcall::ValueBox& box);
    runtime::Value dom_method(uint32_t webcall_id, const std::u16string& name);

    // --- EventTarget ---
    struct Listener {
        runtime::Value callback;
        bool           capture = false;
    };
    void           add_listener(malibu::NodeHandle node, const std::u16string& type,
                                runtime::Value cb, bool capture);
    void           remove_listener(malibu::NodeHandle node, const std::u16string& type,
                                   runtime::Value cb, bool capture);
    runtime::Value make_event(const std::u16string& type, bool bubbles, bool cancelable);

    // --- DOM API helpers ---
    runtime::Value make_class_list(malibu::NodeHandle h);
    runtime::Value make_style(malibu::NodeHandle h);
    runtime::Value make_dataset(malibu::NodeHandle h);
    bool           dispatch_value(malibu::NodeHandle target, runtime::Value event);
    void           fire_at(malibu::NodeHandle node, const std::u16string& type,
                           runtime::Value event, bool fire_capture, bool fire_bubble,
                           bool& stop, bool& stop_now);
    void           install_event_target();
    void           notify_mutation(malibu::NodeHandle node);

    ContextProvider                       context_provider_;
    RectProvider                          rect_provider_;
    MutationHandler                       mutation_handler_;
    CookieGetter                          cookie_getter_;
    CookieSetter                          cookie_setter_;
    SubmitHandler                         submit_handler_;
    runtime::Interpreter&                 interp_;
    malibu::dom::DOMTree&                  tree_;
    malibu::NodeHandle                     document_root_;
    malibu::webcall::RealmState            realm_;
    malibu::webcall::WebCallContext        ctx_;
    malibu::js::bytecode::CallSiteTable    call_sites_;

    // NodeHandle (encoded) -> event type -> listeners. JS callbacks live in the
    // JS layer (the OS-agnostic DOM core never holds Values).
    std::unordered_map<uint64_t,
        std::unordered_map<std::u16string, std::vector<Listener>>> listeners_;
    std::unordered_map<uint64_t, malibu::NodeHandle> iframe_documents_;
    malibu::NodeHandle focused_element_ =
        malibu::NodeHandle::null_handle();

    struct MediaState {
        double current_time = 0.0;
        double duration = std::numeric_limits<double>::quiet_NaN();
        double volume = 1.0;
        double playback_rate = 1.0;
        double default_playback_rate = 1.0;
        bool paused = true;
        bool ended = false;
        bool muted = false;
        int network_state = 0;
        int ready_state = 0;
    };
    std::unordered_map<uint64_t, MediaState> media_states_;
};

} // namespace malibu::js
