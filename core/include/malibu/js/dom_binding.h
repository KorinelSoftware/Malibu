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

    ContextProvider                       context_provider_;
    RectProvider                          rect_provider_;
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
};

} // namespace malibu::js
