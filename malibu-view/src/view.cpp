// malibu-view/src/view.cpp
// MalibuView: end-to-end embedding engine (HTML -> DOM -> CSS -> JS -> layout
// -> raster) with eval, messaging, request interception, and sandboxing.

#include "malibu/view/view.h"
#include "malibu/html/html_parser.h"
#include "malibu/css/parser/css_parser.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace malibu::view {
namespace {
// UTF-16 -> UTF-8 (proper encoding, incl. surrogate pairs).
std::string narrow(const std::u16string& s) {
    std::string r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char32_t cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i+1] >= 0xDC00 && s[i+1] <= 0xDFFF)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (cp < 0x80) r.push_back(static_cast<char>(cp));
        else if (cp < 0x800) { r.push_back(static_cast<char>(0xC0 | (cp >> 6))); r.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { r.push_back(static_cast<char>(0xE0 | (cp >> 12))); r.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); r.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else { r.push_back(static_cast<char>(0xF0 | (cp >> 18))); r.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); r.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); r.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    }
    return r;
}
// UTF-8 -> UTF-16 (proper decoding; invalid sequences become U+FFFD). Web bytes
// are UTF-8 by default, so this is what makes accents/CJK/emoji render at all.
std::u16string widen(const std::string& s) {
    std::u16string out; out.reserve(s.size());
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp; int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { out.push_back(0xFFFD); ++i; continue; }
        if (i + static_cast<size_t>(extra) >= n) { out.push_back(0xFFFD); break; }
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); ++i; continue; }
        i += static_cast<size_t>(extra) + 1;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { out.push_back(0xFFFD); continue; }
        if (cp <= 0xFFFF) out.push_back(static_cast<char16_t>(cp));
        else { cp -= 0x10000; out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10))); out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF))); }
    }
    return out;
}
}  // namespace

View::View() {
    origin_ = security::Origin::parse("about:blank");
    if (fonts_.available()) {
        measurer_ = std::make_unique<text::FreeTypeTextMeasurer>(fonts_);
        drawer_ = std::make_unique<text::FreeTypeTextDrawer>(fonts_);
        layout_.set_text_measurer(measurer_.get());  // real font metrics in layout
    }
    reset_document();
}

View::~View() = default;

void View::reset_document() {
    engine_.interpreter().clear_dom_wrappers();   // old document's node wrappers are stale
    doc_ = std::make_unique<dom::Document>();
    tree_ = std::make_unique<dom::DOMTree>(*doc_);
    resolver_ = std::make_unique<css::StyleResolver>();
    binding_ = std::make_unique<js::DomBinding>(engine_.interpreter(), *tree_, doc_->root());
    binding_->install();
    binding_->set_context_provider([this](malibu::NodeHandle node, const std::u16string& type) {
        return make_canvas_context(node, type);
    });
    // getBoundingClientRect / offset* / client*: lay out on demand (scripts run
    // before the first render) and read the element's layout box.
    binding_->set_rect_provider([this](malibu::NodeHandle n, float& x, float& y, float& w, float& h) -> bool {
        if (!layout_.root()) { apply_styles(); layout_.layout_document(*doc_, static_cast<float>(last_vw_), static_cast<float>(last_vh_)); }
        layout::LayoutBox* b = layout_.box_for_node(n);
        if (!b) return false;
        x = b->x; y = b->y; w = b->width; h = b->height; return true;
    });
    canvases_.clear();
    install_view_globals();
}

void View::install_view_globals() {
    using js::runtime::Interpreter;
    using js::runtime::Value;
    using js::runtime::JSObject;
    using js::runtime::JSArray;
    using js::runtime::JSPromise;
    using js::runtime::JSFunction;
    auto& interp = engine_.interpreter();
    View* self = this;

    // JS -> native: window.malibuNativeMessage("...")
    auto* fn = interp.new_native(u"malibuNativeMessage",
        [self](Interpreter& in, Value, std::vector<Value>& a) {
            if (self->message_handler_ && !a.empty())
                self->message_handler_(narrow(in.to_string(a[0])));
            return Value::make_undefined();
        });
    interp.global()->define(u"malibuNativeMessage", Value::make_heap_ptr(fn));

    // ---- Web Storage (localStorage / sessionStorage), per-origin ----
    auto build_storage = [&](std::function<storage::Storage&()> get) -> Value {
        JSObject* s = interp.new_object();
        auto str16 = [](const std::string& x) { return std::u16string(x.begin(), x.end()); };
        auto sync_len = [get, s]() { s->set(u"length", Value::make_int32(static_cast<int32_t>(get().length()))); };
        sync_len();
        s->set(u"getItem", Value::make_heap_ptr(interp.new_native(u"getItem",
            [get, str16](Interpreter& in, Value, std::vector<Value>& a) -> Value {
                auto v = get().get_item(narrow(in.to_string(a.empty() ? Value::make_undefined() : a[0])));
                return v ? in.str(str16(*v)) : Value::make_null();
            })));
        s->set(u"setItem", Value::make_heap_ptr(interp.new_native(u"setItem",
            [get, sync_len](Interpreter& in, Value, std::vector<Value>& a) -> Value {
                if (a.size() >= 2) get().set_item(narrow(in.to_string(a[0])), narrow(in.to_string(a[1])));
                sync_len();
                return Value::make_undefined();
            })));
        s->set(u"removeItem", Value::make_heap_ptr(interp.new_native(u"removeItem",
            [get, sync_len](Interpreter& in, Value, std::vector<Value>& a) -> Value {
                if (!a.empty()) get().remove_item(narrow(in.to_string(a[0])));
                sync_len();
                return Value::make_undefined();
            })));
        s->set(u"clear", Value::make_heap_ptr(interp.new_native(u"clear",
            [get, sync_len](Interpreter&, Value, std::vector<Value>&) -> Value {
                get().clear(); sync_len(); return Value::make_undefined();
            })));
        s->set(u"key", Value::make_heap_ptr(interp.new_native(u"key",
            [get, str16](Interpreter& in, Value, std::vector<Value>& a) -> Value {
                auto k = get().key(a.empty() ? 0 : static_cast<size_t>(in.to_number(a[0])));
                return k ? in.str(str16(*k)) : Value::make_null();
            })));
        return Value::make_heap_ptr(s);
    };
    interp.global()->define(u"localStorage",
        build_storage([self]() -> storage::Storage& { return self->storage_.local_storage(self->origin_); }));
    interp.global()->define(u"sessionStorage",
        build_storage([self]() -> storage::Storage& { return self->storage_.session_storage(self->origin_, ""); }));

    // ---- fetch() -> Promise<Response> (satisfied via the host request handler) ----
    Value json_parse = Value::make_undefined();
    if (Value* j = interp.global()->find(u"JSON"))
        if (j->is_heap_ptr() && j->as_heap_ptr()->kind == js::vm::HeapObject::kJSObject)
            json_parse = static_cast<JSObject*>(j->as_heap_ptr())->get(u"parse");

    auto* fetch_fn = interp.new_native(u"fetch",
        [self, json_parse](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            std::string url = a.empty() ? std::string() : narrow(in.to_string(a[0]));
            JSPromise* p = in.new_promise();
            network::FetchResponse resp;
            bool handled = self->request_handler_ && self->request_handler_(url, resp);
            if (!handled) {
                in.reject_promise(p, in.str(std::string("TypeError: Failed to fetch: ") + url));
                return Value::make_heap_ptr(p);
            }
            std::string body(resp.body.begin(), resp.body.end());
            int32_t status = resp.status ? resp.status : 200;
            JSObject* res = in.new_object();
            res->set(u"status", Value::make_int32(status));
            res->set(u"ok", Value::make_bool(status >= 200 && status < 300));
            res->set(u"url", in.str(url));
            res->set(u"text", Value::make_heap_ptr(in.new_native(u"text",
                [body](Interpreter& in2, Value, std::vector<Value>&) -> Value {
                    JSPromise* tp = in2.new_promise();
                    in2.resolve_promise(tp, in2.str(body));
                    return Value::make_heap_ptr(tp);
                })));
            res->set(u"json", Value::make_heap_ptr(in.new_native(u"json",
                [body, json_parse](Interpreter& in2, Value, std::vector<Value>&) -> Value {
                    JSPromise* jp = in2.new_promise();
                    std::vector<Value> pa{ in2.str(body) };
                    Value parsed = json_parse.is_undefined() ? Value::make_undefined()
                                                             : in2.call(json_parse, Value::make_undefined(), pa);
                    in2.resolve_promise(jp, parsed);
                    return Value::make_heap_ptr(jp);
                })));
            in.resolve_promise(p, Value::make_heap_ptr(res));
            return Value::make_heap_ptr(p);
        });
    interp.global()->define(u"fetch", Value::make_heap_ptr(fetch_fn));

    // ---- location (parsed from the current URL via the URL builtin) ----
    JSObject* location = interp.new_object();
    if (Value* urlctor = interp.global()->find(u"URL")) {
        std::vector<Value> ua{ interp.str(current_url_) };
        Value u = interp.construct(*urlctor, ua);
        if (u.is_heap_ptr() && u.as_heap_ptr()->kind == js::vm::HeapObject::kJSObject) {
            JSObject* uo = static_cast<JSObject*>(u.as_heap_ptr());
            for (const char16_t* k : {u"href", u"protocol", u"host", u"hostname", u"port",
                                      u"pathname", u"search", u"hash", u"origin"})
                location->set(k, uo->get(k));
        }
    }
    location->set(u"reload", Value::make_heap_ptr(interp.new_native(u"reload",
        [self](Interpreter&, Value, std::vector<Value>&) { self->reload(); return Value::make_undefined(); })));
    location->set(u"assign", Value::make_heap_ptr(interp.new_native(u"assign",
        [self](Interpreter& in, Value, std::vector<Value>& a) { if (!a.empty()) self->load_url(narrow(in.to_string(a[0]))); return Value::make_undefined(); })));
    location->set(u"replace", location->get(u"assign"));
    location->set(u"toString", Value::make_heap_ptr(interp.new_native(u"toString",
        [location](Interpreter&, Value, std::vector<Value>&) { return location->get(u"href"); })));
    interp.global()->define(u"location", Value::make_heap_ptr(location));

    // ---- navigator ----
    JSObject* navigator = interp.new_object();
    navigator->set(u"userAgent", interp.str(std::string("Mozilla/5.0 (RiduxOS) Seage/1.0 Malibu/1.0")));
    navigator->set(u"language", interp.str(std::string("en-US")));
    navigator->set(u"platform", interp.str(std::string("Linux x86_64")));
    navigator->set(u"onLine", Value::make_bool(true));
    navigator->set(u"hardwareConcurrency", Value::make_int32(4));
    navigator->set(u"maxTouchPoints", Value::make_int32(0));
    interp.global()->define(u"navigator", Value::make_heap_ptr(navigator));

    // ---- history (wired to the view's back/forward stack) ----
    JSObject* history = interp.new_object();
    history->set(u"length", Value::make_int32(1));
    history->set(u"state", Value::make_null());
    history->set(u"back", Value::make_heap_ptr(interp.new_native(u"back",
        [self](Interpreter&, Value, std::vector<Value>&) { self->go_back(); return Value::make_undefined(); })));
    history->set(u"forward", Value::make_heap_ptr(interp.new_native(u"forward",
        [self](Interpreter&, Value, std::vector<Value>&) { self->go_forward(); return Value::make_undefined(); })));
    history->set(u"go", Value::make_heap_ptr(interp.new_native(u"go",
        [self](Interpreter& in, Value, std::vector<Value>& a) { int n = a.empty() ? 0 : in.to_int32(a[0]); if (n < 0) self->go_back(); else if (n > 0) self->go_forward(); return Value::make_undefined(); })));
    // pushState/replaceState update history.state (SPA routing); no reload.
    auto state_setter = [history](Interpreter& in, Value, std::vector<Value>& a) {
        history->set(u"state", a.empty() ? Value::make_null() : a[0]); (void)in; return Value::make_undefined();
    };
    history->set(u"pushState", Value::make_heap_ptr(interp.new_native(u"pushState", state_setter)));
    history->set(u"replaceState", Value::make_heap_ptr(interp.new_native(u"replaceState", state_setter)));
    interp.global()->define(u"history", Value::make_heap_ptr(history));

    // ---- window / self alias the global object; expose the platform surface ----
    if (Value* gt = interp.global()->find(u"globalThis")) {
        if (gt->is_heap_ptr() && gt->as_heap_ptr()->kind == js::vm::HeapObject::kJSObject) {
            JSObject* win = static_cast<JSObject*>(gt->as_heap_ptr());
            win->set(u"location", Value::make_heap_ptr(location));
            win->set(u"navigator", Value::make_heap_ptr(navigator));
            win->set(u"history", Value::make_heap_ptr(history));
            win->set(u"self", *gt);
            win->set(u"window", *gt);
            // A top-level browsing context: parent/top/frames refer to itself,
            // no opener. (testharness.js walks window.parent, so these must exist.)
            win->set(u"parent", *gt);
            win->set(u"top", *gt);
            win->set(u"frames", *gt);
            win->set(u"opener", Value::make_null());
            win->set(u"length", Value::make_int32(0));
            // Window-level event registration: listeners are collected per type in
            // a hidden `__winEvents` object (kept alive via the global scope) and
            // fired by View::fire_window_event after the document's scripts run —
            // so `window.addEventListener('load'|'DOMContentLoaded', ...)` works.
            JSObject* winEvents = interp.new_object();
            interp.global()->define(u"__winEvents", Value::make_heap_ptr(winEvents));
            JSFunction* addEL = interp.new_native(u"addEventListener",
                [winEvents](Interpreter& in, Value, std::vector<Value>& a) -> Value {
                    if (a.size() < 2) return Value::make_undefined();
                    std::u16string type = in.to_string(a[0]);
                    Value arrv = winEvents->get(type);
                    JSArray* arr;
                    if (arrv.is_heap_ptr() && arrv.as_heap_ptr()->kind == js::vm::HeapObject::kJSArray)
                        arr = static_cast<JSArray*>(arrv.as_heap_ptr());
                    else { arr = in.new_array(); winEvents->set(type, Value::make_heap_ptr(arr)); }
                    arr->elements.push_back(a[1]);
                    return Value::make_undefined();
                });
            JSFunction* removeEL = interp.new_native(u"removeEventListener",
                [winEvents](Interpreter& in, Value, std::vector<Value>& a) -> Value {
                    if (a.size() < 2) return Value::make_undefined();
                    Value arrv = winEvents->get(in.to_string(a[0]));
                    if (arrv.is_heap_ptr() && arrv.as_heap_ptr()->kind == js::vm::HeapObject::kJSArray) {
                        auto& el = static_cast<JSArray*>(arrv.as_heap_ptr())->elements;
                        el.erase(std::remove_if(el.begin(), el.end(), [&](Value v){ return v == a[1]; }), el.end());
                    }
                    return Value::make_undefined();
                });
            win->set(u"addEventListener", Value::make_heap_ptr(addEL));
            win->set(u"removeEventListener", Value::make_heap_ptr(removeEL));
            win->set(u"dispatchEvent", Value::make_heap_ptr(interp.new_native(u"dispatchEvent",
                [](Interpreter&, Value, std::vector<Value>&) { return Value::make_bool(true); })));
            win->set(u"matchMedia", Value::make_heap_ptr(interp.new_native(u"matchMedia", [](Interpreter& in, Value, std::vector<Value>&) -> Value {
                JSObject* mq = in.new_object(); mq->set(u"matches", Value::make_bool(false));
                mq->set(u"addListener", Value::make_heap_ptr(in.new_native(u"addListener", [](Interpreter&, Value, std::vector<Value>&){ return Value::make_undefined(); })));
                mq->set(u"addEventListener", mq->get(u"addListener"));
                return Value::make_heap_ptr(mq); })));

            // Observers (MutationObserver/IntersectionObserver/ResizeObserver):
            // constructible no-throw objects so SPA init code runs. (Records are
            // not yet delivered; the host re-renders after DOM/scroll changes.)
            auto observer_ctor = [&interp](const char16_t* nm) {
                return Value::make_heap_ptr(interp.new_native(nm,
                    [](Interpreter& in, Value, std::vector<Value>&) -> Value {
                        JSObject* o = in.new_object();
                        auto noop = [](Interpreter&, Value, std::vector<Value>&) { return Value::make_undefined(); };
                        o->set(u"observe", Value::make_heap_ptr(in.new_native(u"observe", noop)));
                        o->set(u"unobserve", Value::make_heap_ptr(in.new_native(u"unobserve", noop)));
                        o->set(u"disconnect", Value::make_heap_ptr(in.new_native(u"disconnect", noop)));
                        o->set(u"takeRecords", Value::make_heap_ptr(in.new_native(u"takeRecords",
                            [](Interpreter& i2, Value, std::vector<Value>&) { return Value::make_heap_ptr(i2.new_array()); })));
                        return Value::make_heap_ptr(o);
                    }));
            };
            win->set(u"MutationObserver", observer_ctor(u"MutationObserver"));
            win->set(u"IntersectionObserver", observer_ctor(u"IntersectionObserver"));
            win->set(u"ResizeObserver", observer_ctor(u"ResizeObserver"));
            // CustomEvent(type, {detail, bubbles}) — Event-like with a detail field.
            win->set(u"CustomEvent", Value::make_heap_ptr(interp.new_native(u"CustomEvent",
                [](Interpreter& in, Value, std::vector<Value>& a) -> Value {
                    JSObject* e = in.new_object();
                    e->set(u"type", a.empty() ? in.str("") : a[0]);
                    e->set(u"bubbles", Value::make_bool(false));
                    e->set(u"detail", Value::make_undefined());
                    if (a.size() >= 2 && a[1].is_heap_ptr()) {
                        e->set(u"detail", in.get_prop_public(a[1], u"detail"));
                        Value bv = in.get_prop_public(a[1], u"bubbles");
                        e->set(u"bubbles", Value::make_bool(bv.is_bool() ? bv.as_bool() : (!bv.is_undefined() && !bv.is_null())));
                    }
                    auto noop = [](Interpreter&, Value, std::vector<Value>&) { return Value::make_undefined(); };
                    e->set(u"preventDefault", Value::make_heap_ptr(in.new_native(u"preventDefault", noop)));
                    e->set(u"stopPropagation", Value::make_heap_ptr(in.new_native(u"stopPropagation", noop)));
                    return Value::make_heap_ptr(e);
                })));

            // getComputedStyle(el): a CSSStyleDeclaration of resolved values.
            win->set(u"getComputedStyle", Value::make_heap_ptr(interp.new_native(u"getComputedStyle",
                [this](Interpreter& in, Value, std::vector<Value>& a) -> Value {
                    JSObject* o = in.new_object();
                    malibu::NodeHandle n = a.empty() ? malibu::NodeHandle::null_handle() : binding_->node_of(a[0]);
                    auto* core = doc_->core(n);
                    const css::ComputedStyle* cs = core ? core->computed_style : nullptr;
                    auto put = [&](const char16_t* camel, const char16_t* kebab, const std::string& v) {
                        Value sv = in.str(v); o->set(camel, sv); o->set(kebab, sv);
                    };
                    auto px = [](float v) { char b[32]; std::snprintf(b, sizeof b, "%gpx", v); return std::string(b); };
                    auto rgb = [](css::Color c) { char b[48];
                        if (c.a >= 255) std::snprintf(b, sizeof b, "rgb(%d, %d, %d)", c.r, c.g, c.b);
                        else std::snprintf(b, sizeof b, "rgba(%d, %d, %d, %g)", c.r, c.g, c.b, c.a / 255.0);
                        return std::string(b); };
                    if (cs) {
                        std::string disp = "block";
                        switch (cs->display) {
                            case css::DisplayType::Inline: disp = "inline"; break;
                            case css::DisplayType::InlineBlock: disp = "inline-block"; break;
                            case css::DisplayType::Flex: case css::DisplayType::InlineFlex: disp = "flex"; break;
                            case css::DisplayType::Grid: case css::DisplayType::InlineGrid: disp = "grid"; break;
                            case css::DisplayType::ListItem: disp = "list-item"; break;
                            case css::DisplayType::None: disp = "none"; break;
                            default: disp = "block";
                        }
                        put(u"display", u"display", disp);
                        const char* pos = cs->position == css::PositionType::Relative ? "relative"
                                        : cs->position == css::PositionType::Absolute ? "absolute"
                                        : cs->position == css::PositionType::Fixed ? "fixed"
                                        : cs->position == css::PositionType::Sticky ? "sticky" : "static";
                        put(u"position", u"position", pos);
                        put(u"visibility", u"visibility", cs->visibility == css::VisibilityType::Hidden ? "hidden" : "visible");
                        put(u"color", u"color", rgb(cs->color));
                        put(u"backgroundColor", u"background-color", rgb(cs->background_color));
                        put(u"fontSize", u"font-size", px(cs->font_size));
                        put(u"fontWeight", u"font-weight", cs->font_weight == css::FontWeight::Bold ? "700" : "400");
                        put(u"lineHeight", u"line-height", px(cs->line_height * cs->font_size));
                        { char b[16]; std::snprintf(b, sizeof b, "%g", cs->opacity); put(u"opacity", u"opacity", b); }
                        put(u"textAlign", u"text-align", cs->text_align == css::TextAlign::Center ? "center"
                                        : cs->text_align == css::TextAlign::Right ? "right" : "left");
                    }
                    if (auto* b = layout_.box_for_node(n)) {
                        put(u"width", u"width", px(b->width));
                        put(u"height", u"height", px(b->height));
                    }
                    o->set(u"getPropertyValue", Value::make_heap_ptr(in.new_native(u"getPropertyValue",
                        [o](Interpreter& i2, Value, std::vector<Value>& ar) -> Value {
                            if (ar.empty()) return i2.str("");
                            Value v = o->get(i2.to_string(ar[0]));
                            return v.is_undefined() ? i2.str("") : v;
                        })));
                    return Value::make_heap_ptr(o);
                })));
        }
        interp.global()->define(u"window", *gt);
        interp.global()->define(u"self", *gt);
    }
    install_wasm_globals();

    // DOM interface objects + constants + document.implementation. Tons of WPT
    // tests (and real pages) reference these for node-type constants, instanceof
    // checks, and document.implementation.* — without them tests error before any
    // assertion. Defined in JS as a prelude so they exist on every document.
    engine_.evaluate(R"JS((function(g){
      var NC={ELEMENT_NODE:1,ATTRIBUTE_NODE:2,TEXT_NODE:3,CDATA_SECTION_NODE:4,ENTITY_REFERENCE_NODE:5,ENTITY_NODE:6,PROCESSING_INSTRUCTION_NODE:7,COMMENT_NODE:8,DOCUMENT_NODE:9,DOCUMENT_TYPE_NODE:10,DOCUMENT_FRAGMENT_NODE:11,NOTATION_NODE:12,DOCUMENT_POSITION_DISCONNECTED:1,DOCUMENT_POSITION_PRECEDING:2,DOCUMENT_POSITION_FOLLOWING:4,DOCUMENT_POSITION_CONTAINS:8,DOCUMENT_POSITION_CONTAINED_BY:16,DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC:32};
      function Node(){} for(var k in NC) Node[k]=NC[k]; g.Node=Node;
      var names=['Element','Document','HTMLDocument','XMLDocument','DocumentFragment','CharacterData','Text','Comment','CDATASection','ProcessingInstruction','Attr','NodeList','HTMLCollection','NamedNodeMap','DocumentType','DOMImplementation','HTMLElement','HTMLDivElement','HTMLSpanElement','HTMLInputElement','HTMLAnchorElement','HTMLImageElement','HTMLButtonElement','HTMLParagraphElement','HTMLUnknownElement','SVGElement','Event','UIEvent','MouseEvent','KeyboardEvent','EventTarget','DOMTokenList','DOMException','DOMStringMap','ShadowRoot','Range','StaticRange','AbstractRange','NodeIterator','TreeWalker','XMLSerializer'];
      for(var i=0;i<names.length;i++){ if(!g[names[i]]) g[names[i]]=function(){}; }
      function NodeFilter(){} NodeFilter.SHOW_ALL=0xFFFFFFFF;NodeFilter.SHOW_ELEMENT=1;NodeFilter.SHOW_ATTRIBUTE=2;NodeFilter.SHOW_TEXT=4;NodeFilter.SHOW_CDATA_SECTION=8;NodeFilter.SHOW_ENTITY_REFERENCE=16;NodeFilter.SHOW_ENTITY=32;NodeFilter.SHOW_PROCESSING_INSTRUCTION=64;NodeFilter.SHOW_COMMENT=128;NodeFilter.SHOW_DOCUMENT=256;NodeFilter.SHOW_DOCUMENT_TYPE=512;NodeFilter.SHOW_DOCUMENT_FRAGMENT=1024;NodeFilter.SHOW_NOTATION=2048;NodeFilter.FILTER_ACCEPT=1;NodeFilter.FILTER_REJECT=2;NodeFilter.FILTER_SKIP=3; g.NodeFilter=NodeFilter;
      if(g.document && !g.document.implementation){
        g.document.implementation={hasFeature:function(){return true;},createHTMLDocument:function(){return g.document;},createDocument:function(){return g.document;},createDocumentType:function(n,p,s){return {name:n,publicId:p||'',systemId:s||'',nodeType:10,nodeName:n};}};
      }
    })(globalThis);)JS", "about:dom-prelude");

    // TreeWalker + NodeIterator (WHATWG DOM traversal), implemented in JS over the
    // engine's node properties — a self-contained WPT cluster (dom/traversal).
    engine_.evaluate(R"JS((function(g){
      function mkfilter(whatToShow, filter){
        return function(n){
          if(!((whatToShow >>> (n.nodeType-1)) & 1)) return 3;
          if(filter==null) return 1;
          return typeof filter==='function' ? filter(n) : filter.acceptNode(n);
        };
      }
      if(g.document) g.document.createTreeWalker=function(root,whatToShow,filter){
        if(root==null) throw new TypeError('root is required');
        whatToShow=(whatToShow===undefined||whatToShow===null)?0xFFFFFFFF:(whatToShow>>>0);
        var f=mkfilter(whatToShow,filter==null?null:filter);
        var tw={root:root,whatToShow:whatToShow,filter:(filter==null?null:filter),currentNode:root};
        function traverseChildren(first){
          var node = first? tw.currentNode.firstChild : tw.currentNode.lastChild;
          while(node){
            var r=f(node);
            if(r===1){ tw.currentNode=node; return node; }
            if(r!==2){ var c=first?node.firstChild:node.lastChild; if(c){ node=c; continue; } }
            while(node){ var sib=first?node.nextSibling:node.previousSibling; if(sib){ node=sib; break; } var p=node.parentNode; if(p==null||p===tw.root||p===tw.currentNode) return null; node=p; }
          }
          return null;
        }
        function traverseSiblings(next){
          var node=tw.currentNode; if(node===tw.root) return null;
          while(true){
            var sib=next?node.nextSibling:node.previousSibling;
            while(sib){ node=sib; var r=f(node); if(r===1){ tw.currentNode=node; return node; } sib=next?node.firstChild:node.lastChild; if(r===2||!sib) sib=next?node.nextSibling:node.previousSibling; }
            node=node.parentNode; if(node==null||node===tw.root) return null; if(f(node)===1) return null;
          }
        }
        tw.parentNode=function(){ var node=tw.currentNode; while(node!=null&&node!==tw.root){ node=node.parentNode; if(node!=null&&f(node)===1){ tw.currentNode=node; return node; } } return null; };
        tw.firstChild=function(){return traverseChildren(true);};
        tw.lastChild=function(){return traverseChildren(false);};
        tw.nextSibling=function(){return traverseSiblings(true);};
        tw.previousSibling=function(){return traverseSiblings(false);};
        tw.nextNode=function(){ var node=tw.currentNode, result=1;
          while(true){ while(result!==2&&node.firstChild){ node=node.firstChild; result=f(node); if(result===1){tw.currentNode=node;return node;} }
            var sib=null,temp=node; while(temp!=null){ if(temp===tw.root) return null; sib=temp.nextSibling; if(sib){break;} temp=temp.parentNode; }
            if(!sib) return null; node=sib; result=f(node); if(result===1){tw.currentNode=node;return node;} } };
        tw.previousNode=function(){ var node=tw.currentNode;
          while(node!==tw.root){ var sib=node.previousSibling; while(sib){ node=sib; var result=f(node); while(result!==2&&node.lastChild){ node=node.lastChild; result=f(node); } if(result===1){tw.currentNode=node;return node;} sib=node.previousSibling; }
            if(node===tw.root||node.parentNode==null) return null; node=node.parentNode; if(f(node)===1){tw.currentNode=node;return node;} } return null; };
        return tw;
      };
      if(g.document) g.document.createNodeIterator=function(root,whatToShow,filter){
        if(root==null) throw new TypeError('root is required');
        whatToShow=(whatToShow===undefined||whatToShow===null)?0xFFFFFFFF:(whatToShow>>>0);
        var f=mkfilter(whatToShow,filter==null?null:filter);
        function nextIn(node){ if(node.firstChild) return node.firstChild; while(node!=null&&node!==root){ if(node.nextSibling) return node.nextSibling; node=node.parentNode; } return null; }
        function prevIn(node){ if(node===root) return null; if(node.previousSibling){ node=node.previousSibling; while(node.lastChild) node=node.lastChild; return node; } return node.parentNode; }
        var ni={root:root,whatToShow:whatToShow,filter:(filter==null?null:filter),referenceNode:root,pointerBeforeReferenceNode:true};
        ni.nextNode=function(){ var node=ni.referenceNode, before=ni.pointerBeforeReferenceNode;
          while(true){ if(!before){ node=nextIn(node); if(node==null) return null; } before=false; if(f(node)===1){ ni.referenceNode=node; ni.pointerBeforeReferenceNode=false; return node; } } };
        ni.previousNode=function(){ var node=ni.referenceNode, before=ni.pointerBeforeReferenceNode;
          while(true){ if(before){ node=prevIn(node); if(node==null) return null; } before=true; if(f(node)===1){ ni.referenceNode=node; ni.pointerBeforeReferenceNode=true; return node; } } };
        ni.detach=function(){};
        return ni;
      };
    })(globalThis);)JS", "about:dom-traversal");
}

// ---------------------------------------------------------------------------
// WebAssembly JS API over MalibuWASM. Decoupled: the JS engine knows nothing
// about WASM; this binding (the integration layer) drives MalibuWASM and bridges
// values. Memory is synced JS<->WASM around each exported call.
// ---------------------------------------------------------------------------
void View::install_wasm_globals() {
    using js::runtime::Interpreter;
    using js::runtime::Value;
    using js::runtime::JSObject;
    using js::runtime::JSArray;
    using js::runtime::JSArrayBuffer;
    using js::runtime::JSTypedArray;
    namespace mw = malibu::wasm;
    auto& interp = engine_.interpreter();
    View* self = this;

    // Extract the raw bytes from a Uint8Array / ArrayBuffer / Array argument.
    auto bytes_of = [](Interpreter& in, Value v) -> std::vector<uint8_t> {
        std::vector<uint8_t> out;
        if (!v.is_heap_ptr()) return out;
        auto* h = v.as_heap_ptr();
        if (h->kind == js::vm::HeapObject::kArrayBuffer) {
            auto* ab = static_cast<JSArrayBuffer*>(h);
            out = ab->data;
        } else if (h->kind == js::vm::HeapObject::kTypedArray) {
            auto* ta = static_cast<JSTypedArray*>(h);
            if (ta->buffer) out.assign(ta->buffer->data.begin() + ta->byte_offset,
                                       ta->buffer->data.begin() + ta->byte_offset + ta->byte_length());
        } else if (h->kind == js::vm::HeapObject::kJSArray) {
            for (Value e : static_cast<JSArray*>(h)->elements) out.push_back(static_cast<uint8_t>(in.to_number(e)));
        }
        return out;
    };
    auto js_from_wasm = [](Interpreter&, mw::Value v) -> Value {
        switch (v.type) {
            case mw::ValType::I32: return Value::make_int32(v.i32);
            case mw::ValType::I64: return Value::make_double(static_cast<double>(v.i64));
            case mw::ValType::F32: return Value::make_double(static_cast<double>(v.f32));
            case mw::ValType::F64: return Value::make_double(v.f64);
            default: return Value::make_undefined();
        }
    };
    auto wasm_from_js = [](Interpreter& in, Value v, mw::ValType t) -> mw::Value {
        double d = in.to_number(v);
        switch (t) {
            case mw::ValType::I32: return mw::Value::I32(static_cast<int32_t>(static_cast<int64_t>(d)));
            case mw::ValType::I64: return mw::Value::I64(static_cast<int64_t>(d));
            case mw::ValType::F32: return mw::Value::F32(static_cast<float>(d));
            default: return mw::Value::F64(d);
        }
    };

    JSObject* WA = interp.new_object();

    // WebAssembly.Module(bytes): decode + store; wrapper holds the module index.
    JSObject* moduleProto = interp.new_object();
    auto make_module = [self, bytes_of, moduleProto](Interpreter& in, std::vector<Value>& a) -> Value {
        std::vector<uint8_t> bytes = bytes_of(in, a.empty() ? Value::make_undefined() : a[0]);
        auto dr = mw::decode(bytes.data(), bytes.size());
        if (!dr.ok()) in.throw_error(u"CompileError", std::u16string(u"WebAssembly.Module: ") +
                                     std::u16string(dr.error.begin(), dr.error.end()));
        self->wasm_modules_.push_back(std::move(dr.module));
        JSObject* obj = in.new_object();
        obj->proto = moduleProto;
        obj->set(u"%wasmModule%", Value::make_int32(static_cast<int32_t>(self->wasm_modules_.size() - 1)), false);
        return Value::make_heap_ptr(obj);
    };
    WA->set(u"Module", Value::make_heap_ptr(interp.new_native(u"Module",
        [make_module](Interpreter& in, Value, std::vector<Value>& a) { return make_module(in, a); }, 1)));

    // WebAssembly.Instance(module, importObject): instantiate + build exports.
    auto make_instance = [self, &interp, js_from_wasm, wasm_from_js](Interpreter& in, std::vector<Value>& a) -> Value {
        Value modv = a.empty() ? Value::make_undefined() : a[0];
        if (!modv.is_heap_ptr()) in.throw_error(u"TypeError", u"WebAssembly.Instance: bad module");
        Value idx = in.get_prop_public(modv, u"%wasmModule%");
        int mi = idx.is_int32() ? idx.as_int32() : -1;
        if (mi < 0 || mi >= static_cast<int>(self->wasm_modules_.size()))
            in.throw_error(u"TypeError", u"WebAssembly.Instance: not a Module");
        mw::Module* mod = self->wasm_modules_[mi].get();

        // Bind imported functions from importObject[module][name].
        Value importObj = a.size() > 1 ? a[1] : Value::make_undefined();
        std::vector<mw::HostFn> hosts;
        for (auto& imp : mod->func_imports) {
            Value jsfn = Value::make_undefined();
            if (importObj.is_heap_ptr()) {
                Value ns = in.get_prop_public(importObj, std::u16string(imp.first.begin(), imp.first.end()));
                if (ns.is_heap_ptr()) jsfn = in.get_prop_public(ns, std::u16string(imp.second.begin(), imp.second.end()));
            }
            if (in.is_callable(jsfn)) interp.add_host_root(jsfn);
            mw::HostFn fn = [self, jsfn, js_from_wasm](const std::vector<mw::Value>& args) -> std::vector<mw::Value> {
                auto& in2 = self->engine_.interpreter();
                if (!in2.is_callable(jsfn)) return {};
                std::vector<Value> jargs;
                for (auto& w : args) jargs.push_back(js_from_wasm(in2, w));
                in2.call(jsfn, Value::make_undefined(), jargs);
                return {};  // host result coercion (single return) is a follow-up
            };
            hosts.push_back(std::move(fn));
        }

        std::string err;
        auto inst = mw::instantiate(*mod, hosts, err);
        if (!inst) in.throw_error(u"LinkError", std::u16string(err.begin(), err.end()));
        self->wasm_instances_.push_back(std::move(inst));
        int ii = static_cast<int>(self->wasm_instances_.size() - 1);
        mw::Instance* instp = self->wasm_instances_[ii].get();

        // Linear memory: a JS ArrayBuffer that mirrors WASM memory. The exported
        // functions sync JS->WASM before a call and WASM->JS after, so a
        // TypedArray over `memory.buffer` reflects (and feeds) the WASM heap.
        JSArrayBuffer* memAb = nullptr;
        if (mod->has_memory) {
            memAb = interp.heap().alloc<JSArrayBuffer>();
            memAb->proto = nullptr;
            memAb->data = instp->memory().data;
            interp.push_root(Value::make_heap_ptr(memAb));  // (kept alive via exports below)
        }

        JSObject* exports = in.new_object();
        for (auto& e : mod->exports) {
            if (e.kind != 0) continue;  // (only function exports are bridged here)
            uint32_t fidx = e.index;
            const mw::FuncType& ft = mod->types[instp->funcs()[fidx].type_index];
            std::vector<mw::ValType> params = ft.params;
            auto native = interp.new_native(std::u16string(e.name.begin(), e.name.end()),
                [self, ii, fidx, params, memAb, js_from_wasm, wasm_from_js](Interpreter& in2, Value, std::vector<Value>& ca) -> Value {
                    mw::Instance* ip = self->wasm_instances_[ii].get();
                    if (memAb) {  // JS -> WASM: feed any JS writes into the heap
                        ip->memory().data = memAb->data;
                    }
                    std::vector<mw::Value> wargs;
                    for (size_t k = 0; k < params.size(); ++k)
                        wargs.push_back(wasm_from_js(in2, k < ca.size() ? ca[k] : Value::make_undefined(), params[k]));
                    std::string e2;
                    auto rets = ip->invoke(fidx, wargs, e2);
                    if (memAb) memAb->data = ip->memory().data;  // WASM -> JS
                    if (!rets) in2.throw_error(u"RuntimeError", std::u16string(e2.begin(), e2.end()));
                    if (rets->empty()) return Value::make_undefined();
                    return js_from_wasm(in2, (*rets)[0]);
                });
            exports->set(std::u16string(e.name.begin(), e.name.end()), Value::make_heap_ptr(native));
        }
        if (memAb) {
            JSObject* memObj = in.new_object();
            memObj->set(u"buffer", Value::make_heap_ptr(memAb));
            exports->set(u"memory", Value::make_heap_ptr(memObj));
            interp.pop_root();
        }
        JSObject* obj = in.new_object();
        obj->set(u"exports", Value::make_heap_ptr(exports));
        return Value::make_heap_ptr(obj);
    };
    WA->set(u"Instance", Value::make_heap_ptr(interp.new_native(u"Instance",
        [make_instance](Interpreter& in, Value, std::vector<Value>& a) { return make_instance(in, a); }, 2)));

    // WebAssembly.compile / instantiate (promise forms).
    WA->set(u"compile", Value::make_heap_ptr(interp.new_native(u"compile",
        [make_module](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            auto* p = in.new_promise();
            try { in.resolve_promise(p, make_module(in, a)); }
            catch (js::runtime::ThrowSignal& s) { in.reject_promise(p, s.value); }
            return Value::make_heap_ptr(p); }, 1)));
    WA->set(u"instantiate", Value::make_heap_ptr(interp.new_native(u"instantiate",
        [make_module, make_instance](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            auto* p = in.new_promise();
            try {
                std::vector<Value> ma{ a.empty() ? Value::make_undefined() : a[0] };
                Value module = make_module(in, ma);
                std::vector<Value> ia{ module, a.size() > 1 ? a[1] : Value::make_undefined() };
                Value instance = make_instance(in, ia);
                JSObject* result = in.new_object();
                result->set(u"module", module);
                result->set(u"instance", instance);
                in.resolve_promise(p, Value::make_heap_ptr(result));
            } catch (js::runtime::ThrowSignal& s) { in.reject_promise(p, s.value); }
            return Value::make_heap_ptr(p); }, 2)));

    interp.global()->define(u"WebAssembly", Value::make_heap_ptr(WA));
}

// CanvasRenderingContext2D over MalibuCanvas. The context is a Proxy so that
// `ctx.fillStyle = "red"` / `ctx.lineWidth = 3` route to the surface (the same
// generalized host-object mechanism as el.style), while methods come from the
// target object.
js::runtime::Value View::make_canvas_context(malibu::NodeHandle node, const std::u16string& type) {
    using js::runtime::Interpreter;
    using js::runtime::Value;
    using js::runtime::JSObject;
    namespace cv = malibu::canvas;
    auto& interp = engine_.interpreter();
    uint64_t key = (static_cast<uint64_t>(node.index) << 32) | node.generation;
    int cw = 300, ch = 150;
    if (auto a = tree_->get_attribute(node, u"width")) { int v = std::atoi(narrow(*a).c_str()); if (v > 0) cw = v; }
    if (auto a = tree_->get_attribute(node, u"height")) { int v = std::atoi(narrow(*a).c_str()); if (v > 0) ch = v; }

    if (type == u"webgl" || type == u"webgl2" || type == u"experimental-webgl") {
        auto git = gl_contexts_.find(key);
        if (git == gl_contexts_.end())
            git = gl_contexts_.emplace(key, std::make_shared<malibu::gl::Context>(cw, ch)).first;
        return make_webgl_context(git->second.get());
    }
    if (type != u"2d") return Value::make_null();
    auto it = canvases_.find(key);
    if (it == canvases_.end())
        it = canvases_.emplace(key, std::make_shared<cv::Canvas2D>(cw, ch)).first;
    cv::Canvas2D* c = it->second.get();

    JSObject* t = interp.new_object();
    auto m = [&](const char16_t* name, js::runtime::NativeFn fn) {
        t->set(name, Value::make_heap_ptr(interp.new_native(name, std::move(fn))));
    };
    auto num = [](Interpreter& in, std::vector<Value>& a, size_t i) { return i < a.size() ? in.to_number(a[i]) : 0.0; };
    m(u"fillRect",   [c, num](Interpreter& in, Value, std::vector<Value>& a) -> Value { c->fill_rect(num(in,a,0),num(in,a,1),num(in,a,2),num(in,a,3)); return Value::make_undefined(); });
    m(u"clearRect",  [c, num](Interpreter& in, Value, std::vector<Value>& a) -> Value { c->clear_rect(num(in,a,0),num(in,a,1),num(in,a,2),num(in,a,3)); return Value::make_undefined(); });
    m(u"strokeRect", [c, num](Interpreter& in, Value, std::vector<Value>& a) -> Value { c->stroke_rect(num(in,a,0),num(in,a,1),num(in,a,2),num(in,a,3)); return Value::make_undefined(); });
    m(u"beginPath",  [c](Interpreter&, Value, std::vector<Value>&) -> Value { c->begin_path(); return Value::make_undefined(); });
    m(u"closePath",  [c](Interpreter&, Value, std::vector<Value>&) -> Value { c->close_path(); return Value::make_undefined(); });
    m(u"moveTo",     [c, num](Interpreter& in, Value, std::vector<Value>& a) -> Value { c->move_to(num(in,a,0),num(in,a,1)); return Value::make_undefined(); });
    m(u"lineTo",     [c, num](Interpreter& in, Value, std::vector<Value>& a) -> Value { c->line_to(num(in,a,0),num(in,a,1)); return Value::make_undefined(); });
    m(u"rect",       [c, num](Interpreter& in, Value, std::vector<Value>& a) -> Value { c->rect(num(in,a,0),num(in,a,1),num(in,a,2),num(in,a,3)); return Value::make_undefined(); });
    m(u"arc",        [c, num](Interpreter& in, Value, std::vector<Value>& a) -> Value { c->arc(num(in,a,0),num(in,a,1),num(in,a,2),num(in,a,3),num(in,a,4), a.size()>5 && in.to_bool(a[5])); return Value::make_undefined(); });
    m(u"fill",       [c](Interpreter&, Value, std::vector<Value>&) -> Value { c->fill(); return Value::make_undefined(); });
    m(u"stroke",     [c](Interpreter&, Value, std::vector<Value>&) -> Value { c->stroke(); return Value::make_undefined(); });
    m(u"save",       [](Interpreter&, Value, std::vector<Value>&) -> Value { return Value::make_undefined(); });
    m(u"restore",    [](Interpreter&, Value, std::vector<Value>&) -> Value { return Value::make_undefined(); });
    // current style values, mirrored on the target for reads.
    t->set(u"fillStyle", interp.str(std::string("#000000")));
    t->set(u"strokeStyle", interp.str(std::string("#000000")));
    t->set(u"lineWidth", Value::make_double(1.0));
    t->set(u"globalAlpha", Value::make_double(1.0));

    // Proxy: route style sets to the surface.
    JSObject* handler = interp.new_object();
    handler->set(u"set", Value::make_heap_ptr(interp.new_native(u"set",
        [c](Interpreter& in, Value, std::vector<Value>& a) -> Value {
            if (a.size() < 3) return Value::make_bool(true);
            JSObject* target = a[0].is_heap_ptr() ? static_cast<JSObject*>(a[0].as_heap_ptr()) : nullptr;
            std::u16string k = in.to_string(a[1]);
            if (k == u"fillStyle")        c->set_fill_style(narrow(in.to_string(a[2])));
            else if (k == u"strokeStyle") c->set_stroke_style(narrow(in.to_string(a[2])));
            else if (k == u"lineWidth")   c->set_line_width(in.to_number(a[2]));
            else if (k == u"globalAlpha") c->set_global_alpha(in.to_number(a[2]));
            if (target) target->set(k, a[2]);  // mirror for reads
            return Value::make_bool(true);
        })));
    auto* px = interp.heap().alloc<js::runtime::JSProxy>();
    px->target = Value::make_heap_ptr(t);
    px->handler = Value::make_heap_ptr(handler);
    return Value::make_heap_ptr(px);
}

// WebGLRenderingContext over MalibuGL. GL object ids are JS numbers.
js::runtime::Value View::make_webgl_context(malibu::gl::Context* g) {
    using js::runtime::Interpreter;
    using js::runtime::Value;
    using js::runtime::JSObject;
    using js::runtime::JSArrayBuffer;
    using js::runtime::JSTypedArray;
    auto& interp = engine_.interpreter();
    JSObject* gl = interp.new_object();
    auto C = [&](const char16_t* n, int v) { gl->set(n, Value::make_int32(v)); };
    C(u"VERTEX_SHADER", 0x8B31); C(u"FRAGMENT_SHADER", 0x8B30);
    C(u"ARRAY_BUFFER", 0x8892); C(u"ELEMENT_ARRAY_BUFFER", 0x8893);
    C(u"STATIC_DRAW", 0x88E4); C(u"DYNAMIC_DRAW", 0x88E8);
    C(u"FLOAT", 0x1406); C(u"UNSIGNED_BYTE", 0x1401); C(u"UNSIGNED_SHORT", 0x1403);
    C(u"COLOR_BUFFER_BIT", 0x4000); C(u"DEPTH_BUFFER_BIT", 0x0100);
    C(u"TRIANGLES", 4); C(u"TRIANGLE_STRIP", 5); C(u"TRIANGLE_FAN", 6); C(u"POINTS", 0); C(u"LINES", 1);
    C(u"COMPILE_STATUS", 0x8B81); C(u"LINK_STATUS", 0x8B82);
    C(u"DEPTH_TEST", 0x0B71); C(u"BLEND", 0x0BE2);
    C(u"TEXTURE_2D", 0x0DE1); C(u"TEXTURE0", 0x84C0); C(u"RGBA", 0x1908); C(u"RGB", 0x1907);
    C(u"NEAREST", 0x2600); C(u"LINEAR", 0x2601);
    C(u"TEXTURE_MAG_FILTER", 0x2800); C(u"TEXTURE_MIN_FILTER", 0x2801);
    C(u"TEXTURE_WRAP_S", 0x2802); C(u"TEXTURE_WRAP_T", 0x2803); C(u"CLAMP_TO_EDGE", 0x812F);

    auto bytes_of = [](Interpreter&, Value v, std::vector<uint8_t>& out) {
        if (!v.is_heap_ptr()) return;
        auto* h = v.as_heap_ptr();
        if (h->kind == js::vm::HeapObject::kTypedArray) { auto* ta = static_cast<JSTypedArray*>(h); if (ta->buffer) out.assign(ta->buffer->data.begin()+ta->byte_offset, ta->buffer->data.begin()+ta->byte_offset+ta->byte_length()); }
        else if (h->kind == js::vm::HeapObject::kArrayBuffer) out = static_cast<JSArrayBuffer*>(h)->data;
    };
    auto floats_of = [bytes_of](Interpreter& in, Value v) { std::vector<uint8_t> b; bytes_of(in, v, b); std::vector<float> f(b.size()/4); if (!b.empty()) std::memcpy(f.data(), b.data(), f.size()*4); return f; };
    auto I = [](Interpreter& in, std::vector<Value>& a, size_t i) { return i < a.size() ? static_cast<int>(in.to_number(a[i])) : 0; };
    auto F = [](Interpreter& in, std::vector<Value>& a, size_t i) { return i < a.size() ? static_cast<float>(in.to_number(a[i])) : 0.f; };
    auto m = [&](const char16_t* n, js::runtime::NativeFn fn) { gl->set(n, Value::make_heap_ptr(interp.new_native(n, std::move(fn)))); };

    m(u"createShader", [g,I](Interpreter& in, Value, std::vector<Value>& a) { return Value::make_int32((int)g->createShader(I(in,a,0))); });
    m(u"shaderSource", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->shaderSource(I(in,a,0), narrow(in.to_string(a.size()>1?a[1]:Value::make_undefined()))); return Value::make_undefined(); });
    m(u"compileShader", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->compileShader(I(in,a,0)); return Value::make_undefined(); });
    m(u"getShaderParameter", [g,I](Interpreter& in, Value, std::vector<Value>& a) { return Value::make_bool(g->getShaderParameter(I(in,a,0), I(in,a,1))); });
    m(u"getShaderInfoLog", [g,I](Interpreter& in, Value, std::vector<Value>& a) { return in.str(g->getShaderInfoLog(I(in,a,0))); });
    m(u"createProgram", [g](Interpreter&, Value, std::vector<Value>&) { return Value::make_int32((int)g->createProgram()); });
    m(u"attachShader", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->attachShader(I(in,a,0), I(in,a,1)); return Value::make_undefined(); });
    m(u"linkProgram", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->linkProgram(I(in,a,0)); return Value::make_undefined(); });
    m(u"getProgramParameter", [g,I](Interpreter& in, Value, std::vector<Value>& a) { return Value::make_bool(g->getProgramParameter(I(in,a,0), I(in,a,1))); });
    m(u"getProgramInfoLog", [g,I](Interpreter& in, Value, std::vector<Value>& a) { return in.str(g->getProgramInfoLog(I(in,a,0))); });
    m(u"useProgram", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->useProgram(I(in,a,0)); return Value::make_undefined(); });
    m(u"createBuffer", [g](Interpreter&, Value, std::vector<Value>&) { return Value::make_int32((int)g->createBuffer()); });
    m(u"bindBuffer", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->bindBuffer(I(in,a,0), I(in,a,1)); return Value::make_undefined(); });
    m(u"bufferData", [g,I,bytes_of](Interpreter& in, Value, std::vector<Value>& a) -> Value { std::vector<uint8_t> b; bytes_of(in, a.size()>1?a[1]:Value::make_undefined(), b); g->bufferData(I(in,a,0), b.data(), b.size()); return Value::make_undefined(); });
    m(u"getAttribLocation", [g,I](Interpreter& in, Value, std::vector<Value>& a) { return Value::make_int32(g->getAttribLocation(I(in,a,0), narrow(in.to_string(a.size()>1?a[1]:Value::make_undefined())))); });
    m(u"enableVertexAttribArray", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->enableVertexAttribArray(I(in,a,0)); return Value::make_undefined(); });
    m(u"vertexAttribPointer", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->vertexAttribPointer(I(in,a,0), I(in,a,1), I(in,a,2), a.size()>3&&in.to_bool(a[3]), I(in,a,4), I(in,a,5)); return Value::make_undefined(); });
    m(u"getUniformLocation", [g,I](Interpreter& in, Value, std::vector<Value>& a) { return Value::make_int32(g->getUniformLocation(I(in,a,0), narrow(in.to_string(a.size()>1?a[1]:Value::make_undefined())))); });
    m(u"uniform1f", [g,I,F](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->uniform1f(I(in,a,0), F(in,a,1)); return Value::make_undefined(); });
    m(u"uniform2f", [g,I,F](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->uniform2f(I(in,a,0), F(in,a,1), F(in,a,2)); return Value::make_undefined(); });
    m(u"uniform3f", [g,I,F](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->uniform3f(I(in,a,0), F(in,a,1), F(in,a,2), F(in,a,3)); return Value::make_undefined(); });
    m(u"uniform4f", [g,I,F](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->uniform4f(I(in,a,0), F(in,a,1), F(in,a,2), F(in,a,3), F(in,a,4)); return Value::make_undefined(); });
    m(u"uniform1i", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->uniform1i(I(in,a,0), I(in,a,1)); return Value::make_undefined(); });
    m(u"uniformMatrix4fv", [g,I,floats_of](Interpreter& in, Value, std::vector<Value>& a) -> Value { auto f = floats_of(in, a.size()>2?a[2]:Value::make_undefined()); if (f.size()>=16) g->uniformMatrix4fv(I(in,a,0), f.data()); return Value::make_undefined(); });
    m(u"clearColor", [g,F](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->clearColor(F(in,a,0), F(in,a,1), F(in,a,2), F(in,a,3)); return Value::make_undefined(); });
    m(u"clear", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->clear(I(in,a,0)); return Value::make_undefined(); });
    m(u"viewport", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->viewport(I(in,a,0), I(in,a,1), I(in,a,2), I(in,a,3)); return Value::make_undefined(); });
    m(u"drawArrays", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->drawArrays(I(in,a,0), I(in,a,1), I(in,a,2)); return Value::make_undefined(); });
    // textures
    m(u"createTexture", [g](Interpreter&, Value, std::vector<Value>&) { return Value::make_int32((int)g->createTexture()); });
    m(u"bindTexture", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->bindTexture(I(in,a,0), I(in,a,1)); return Value::make_undefined(); });
    m(u"activeTexture", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->activeTexture(I(in,a,0)); return Value::make_undefined(); });
    m(u"texParameteri", [g,I](Interpreter& in, Value, std::vector<Value>& a) -> Value { g->texParameteri(I(in,a,1), I(in,a,2)); return Value::make_undefined(); });
    // texImage2D(target, level, internalformat, width, height, border, format, type, pixels)
    m(u"texImage2D", [g,I,bytes_of](Interpreter& in, Value, std::vector<Value>& a) -> Value {
        std::vector<uint8_t> b;
        if (a.size() == 9) { int w = I(in,a,3), h = I(in,a,4); bytes_of(in, a[8], b); g->texImage2D(w, h, b.data(), b.size()); }
        else if (a.size() >= 6) { bytes_of(in, a[5], b); int side = (int)std::sqrt((double)(b.size()/4)); g->texImage2D(side, side, b.data(), b.size()); }
        return Value::make_undefined(); });
    // No-ops accepted by typical setup code.
    for (const char16_t* n : { u"enable", u"disable", u"blendFunc", u"depthFunc", u"deleteShader", u"deleteProgram", u"deleteBuffer", u"bindFramebuffer", u"activeTexture", u"pixelStorei", u"flush", u"finish" })
        m(n, [](Interpreter&, Value, std::vector<Value>&) { return Value::make_undefined(); });
    return Value::make_heap_ptr(gl);
}

void View::composite_canvases(render::Framebuffer& fb, float scroll_y) {
    const int sy_off = static_cast<int>(scroll_y);
    // Blit a source RGBA bitmap at a node's layout-box origin (over the page).
    auto blit = [&](malibu::NodeHandle node, const std::vector<uint8_t>& src, int cw, int ch) {
        layout::LayoutBox* box = layout_.box_for_node(node);
        if (!box) return;
        int ox = static_cast<int>(box->x), oy = static_cast<int>(box->y) - sy_off;
        for (int y = 0; y < ch; ++y) {
            int dy = oy + y; if (dy < 0 || dy >= fb.height) continue;
            for (int x = 0; x < cw; ++x) {
                int dx = ox + x; if (dx < 0 || dx >= fb.width) continue;
                size_t si = (static_cast<size_t>(y) * cw + x) * 4;
                uint8_t sa = src[si + 3];
                if (sa == 0) continue;
                size_t di = (static_cast<size_t>(dy) * fb.width + dx) * 4;
                double a = sa / 255.0;
                for (int k = 0; k < 3; ++k)
                    fb.rgba[di + k] = static_cast<uint8_t>(src[si + k] * a + fb.rgba[di + k] * (1 - a));
                fb.rgba[di + 3] = 255;
            }
        }
    };
    for (auto& [key, canvas] : canvases_) {
        malibu::NodeHandle node{static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key & 0xFFFFFFFF)};
        blit(node, canvas->pixels(), canvas->width(), canvas->height());
    }
    for (auto& [key, ctx] : gl_contexts_) {
        malibu::NodeHandle node{static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key & 0xFFFFFFFF)};
        blit(node, ctx->pixels(), ctx->width(), ctx->height());
    }
    // <img> bitmaps, scaled (nearest) to the element's content box.
    for (auto& [key, img] : images_) {
        if (!img.ok || img.width == 0 || img.height == 0) continue;
        malibu::NodeHandle node{static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key & 0xFFFFFFFF)};
        layout::LayoutBox* box = layout_.box_for_node(node);
        if (!box) continue;
        int ox = static_cast<int>(box->x), oy = static_cast<int>(box->y) - sy_off;
        int bw = static_cast<int>(box->width > 0 ? box->width : img.width);
        int bh = static_cast<int>(box->height > 0 ? box->height : img.height);
        // object-fit: map box pixels → image pixels via a per-axis scale + offset.
        // `fill` (default) stretches each axis to the box; the others scale
        // uniformly and center, letterboxing (contain) or cropping (cover/none).
        const auto* st = box->style;
        auto fit = st ? st->object_fit : malibu::css::ObjectFit::Fill;
        double sxk = static_cast<double>(img.width) / bw;   // src px per dst px, x
        double syk = static_cast<double>(img.height) / bh;  // src px per dst px, y
        double offx = 0, offy = 0;                          // dst offset of the image
        if (fit != malibu::css::ObjectFit::Fill) {
            double scale;  // dst px per src px (uniform)
            double sc_contain = std::min(static_cast<double>(bw) / img.width, static_cast<double>(bh) / img.height);
            double sc_cover   = std::max(static_cast<double>(bw) / img.width, static_cast<double>(bh) / img.height);
            if (fit == malibu::css::ObjectFit::Contain)        scale = sc_contain;
            else if (fit == malibu::css::ObjectFit::Cover)     scale = sc_cover;
            else if (fit == malibu::css::ObjectFit::None)      scale = 1.0;
            else /* ScaleDown */                               scale = std::min(1.0, sc_contain);
            sxk = syk = 1.0 / scale;
            offx = (bw - img.width * scale) / 2.0;   // center (object-position: 50% 50%)
            offy = (bh - img.height * scale) / 2.0;
        }
        for (int y = 0; y < bh; ++y) {
            int dy = oy + y; if (dy < 0 || dy >= fb.height) continue;
            int sy = static_cast<int>((y - offy) * syk);
            if (sy < 0 || sy >= img.height) continue;   // letterbox / crop edge
            for (int x = 0; x < bw; ++x) {
                int dx = ox + x; if (dx < 0 || dx >= fb.width) continue;
                int sx = static_cast<int>((x - offx) * sxk);
                if (sx < 0 || sx >= img.width) continue;
                size_t si = (static_cast<size_t>(sy) * img.width + sx) * 4;
                uint8_t sa = img.rgba[si + 3];
                if (sa == 0) continue;
                size_t di = (static_cast<size_t>(dy) * fb.width + dx) * 4;
                double a = sa / 255.0;
                for (int k = 0; k < 3; ++k)
                    fb.rgba[di + k] = static_cast<uint8_t>(img.rgba[si + k] * a + fb.rgba[di + k] * (1 - a));
                fb.rgba[di + 3] = 255;
            }
        }
    }
}

void View::load_images(const malibu::html::ParsedDocument&) {
    images_.clear();
    if (!request_handler_) return;
    std::vector<malibu::NodeHandle> imgs;
    tree_->query_selector_all(doc_->root(), u"img", imgs);
    // Picks the best URL from an <img>: real src, else the last (largest) srcset
    // candidate, else lazy-load data-src/data-srcset. Skips data: placeholders.
    auto pick_url = [&](malibu::NodeHandle node) -> std::u16string {
        auto srcset_last = [](const std::u16string& ss) -> std::u16string {
            std::u16string best; size_t i = 0;
            while (i < ss.size()) {
                while (i < ss.size() && (ss[i] == u' ' || ss[i] == u',' || ss[i] == u'\n' || ss[i] == u'\t')) ++i;
                size_t st = i; while (i < ss.size() && ss[i] != u' ' && ss[i] != u',') ++i;
                if (i > st) best = ss.substr(st, i - st);          // last candidate = largest
                while (i < ss.size() && ss[i] != u',') ++i;        // skip the descriptor
            }
            return best;
        };
        auto val = [&](const char16_t* n) -> std::u16string { auto a = tree_->get_attribute(node, n); return a ? *a : std::u16string(); };
        std::u16string src = val(u"src");
        if (!src.empty() && src.rfind(u"data:", 0) != 0) return src;
        std::u16string ss = val(u"srcset"); if (ss.empty()) ss = val(u"data-srcset");
        if (!ss.empty()) { auto u = srcset_last(ss); if (!u.empty()) return u; }
        std::u16string ds = val(u"data-src"); if (!ds.empty()) return ds;
        return src;
    };
    for (malibu::NodeHandle node : imgs) {
        std::u16string url = pick_url(node);
        if (url.empty() || url.rfind(u"data:", 0) == 0) continue;
        network::FetchResponse resp;
        if (!request_handler_(resolve_url(url), resp)) continue;
        const auto& body = resp.body;
        // SVG sniff (by extension/content) — vector, so it needs a target size.
        bool is_svg = url.find(u".svg") != std::u16string::npos;
        if (!is_svg && body.size() > 5) {
            std::string head(body.begin(), body.begin() + std::min<size_t>(body.size(), 256));
            is_svg = head.find("<svg") != std::string::npos || head.find("<?xml") != std::string::npos;
        }
        malibu::image::DecodedImage img;
        int wattr = 0, hattr = 0;
        if (auto a = tree_->get_attribute(node, u"width")) wattr = std::atoi(narrow(*a).c_str());
        if (auto a = tree_->get_attribute(node, u"height")) hattr = std::atoi(narrow(*a).c_str());
        if (is_svg) {
            int sw = wattr > 0 ? wattr : (hattr > 0 ? hattr : 64);
            int sh = hattr > 0 ? hattr : (wattr > 0 ? wattr : 64);
            img = malibu::image::decode_svg(body.data(), body.size(), sw, sh);
        } else {
            img = malibu::image::decode_image(body.data(), body.size());
        }
        if (!img.ok) continue;  // (WebP/GIF: a future slice)
        uint64_t key = (static_cast<uint64_t>(node.index) << 32) | node.generation;
        // Replaced-element sizing: set the intrinsic aspect-ratio + only the
        // dimension(s) given (attrs or, as a default, the intrinsic width). The
        // layout derives the other side from the aspect-ratio, and author CSS
        // (max-width:100%, width:…, height:auto) overrides — so images keep their
        // proportions instead of stretching.
        std::string author;
        if (auto s = tree_->get_attribute(node, u"style")) author = narrow(*s);
        // Does author CSS already set the `width`/`height` property (not min-/max-)?
        // If so we must NOT append an intrinsic default that would override it
        // (later declaration wins in one inline style string).
        // True iff author CSS sets `prop` to an explicit (non-auto) value. `auto`
        // doesn't count — e.g. the responsive `max-width:100%;height:auto` pattern
        // must still get an intrinsic width default.
        auto has_dim = [&](const char* prop) {
            std::string low; low.reserve(author.size());
            for (char c : author) low += (c >= 'A' && c <= 'Z') ? char(c + 32) : c;
            std::string p = prop; size_t pos = 0;
            while ((pos = low.find(p, pos)) != std::string::npos) {
                bool before = (pos == 0) || low[pos-1] == ';' || low[pos-1] == ' ';
                size_t a = pos + p.size(); while (a < low.size() && low[a] == ' ') ++a;
                if (before && a < low.size() && low[a] == ':') {
                    size_t v = a + 1; while (v < low.size() && low[v] == ' ') ++v;
                    if (low.compare(v, 4, "auto") != 0) return true;  // explicit size
                }
                pos += p.size();
            }
            return false;
        };
        bool css_w = has_dim("width"), css_h = has_dim("height");
        std::string style = author.empty() ? "" : author + ";";
        style += "display:inline-block;aspect-ratio:" + std::to_string(img.width) + "/" + std::to_string(std::max(1, img.height)) + ";";
        if (!css_w && !css_h) {
            if (wattr > 0 && hattr > 0)  style += "width:" + std::to_string(wattr) + "px;height:" + std::to_string(hattr) + "px";
            else if (wattr > 0)          style += "width:" + std::to_string(wattr) + "px";
            else if (hattr > 0)          style += "height:" + std::to_string(hattr) + "px";
            else                         style += "width:" + std::to_string(img.width) + "px";
        } else {  // author set one dim via CSS; supply the other from an attr if given
            if (wattr > 0 && !css_w) style += "width:" + std::to_string(wattr) + "px";
            if (hattr > 0 && !css_h) style += "height:" + std::to_string(hattr) + "px";
        }
        tree_->set_attribute(node, u"style", widen(style));
        images_[key] = std::move(img);
    }

    // Inline <svg> icons: serialize the subtree back to SVG text and rasterize.
    std::vector<malibu::NodeHandle> svgs;
    tree_->query_selector_all(doc_->root(), u"svg", svgs);
    std::function<std::string(malibu::NodeHandle)> serialize = [&](malibu::NodeHandle n) -> std::string {
        auto* c = doc_->core(n);
        if (!c) return "";
        if (c->node_type == malibu::dom::kTextNode) return narrow(c->text_content);
        if (c->node_type != malibu::dom::kElementNode) return "";
        std::string o = "<" + narrow(c->tag_name);
        for (auto& [k, v] : c->attributes) o += " " + narrow(k) + "=\"" + narrow(v) + "\"";
        o += ">";
        for (auto ch : c->children) o += serialize(ch);
        return o + "</" + narrow(c->tag_name) + ">";
    };
    for (malibu::NodeHandle node : svgs) {
        int sw = 0, sh = 0;
        if (auto a = tree_->get_attribute(node, u"width")) sw = std::atoi(narrow(*a).c_str());
        if (auto a = tree_->get_attribute(node, u"height")) sh = std::atoi(narrow(*a).c_str());
        if (sw <= 0) sw = sh > 0 ? sh : 24;
        if (sh <= 0) sh = sw > 0 ? sw : 24;
        std::string svgtext = serialize(node);
        auto img = malibu::image::decode_svg(reinterpret_cast<const uint8_t*>(svgtext.data()), svgtext.size(), sw, sh);
        if (!img.ok) continue;
        uint64_t key = (static_cast<uint64_t>(node.index) << 32) | node.generation;
        std::string style = "display:inline-block;width:" + std::to_string(sw) + "px;height:" + std::to_string(sh) + "px";
        if (auto s = tree_->get_attribute(node, u"style")) style = narrow(*s) + ";" + style;
        tree_->set_attribute(node, u"style", widen(style));
        images_[key] = std::move(img);
    }

    // Form controls: give <input>/<textarea> a visible box and render their
    // value/placeholder text (so search boxes, buttons etc. appear).
    std::vector<malibu::NodeHandle> inputs;
    tree_->query_selector_all(doc_->root(), u"input", inputs);
    for (malibu::NodeHandle n : inputs) {
        std::u16string type = tree_->get_attribute(n, u"type").value_or(u"text");
        for (auto& ch : type) if (ch >= u'A' && ch <= u'Z') ch += 32;
        if (type == u"hidden") continue;
        std::string st;
        if (auto s = tree_->get_attribute(n, u"style")) st = narrow(*s) + ";";
        st += "display:inline-block;box-sizing:border-box;";
        if (type == u"checkbox" || type == u"radio") {
            st += "width:13px;height:13px;border:1px solid #888;background:#fff;";
        } else if (type == u"submit" || type == u"button" || type == u"reset") {
            st += "border:1px solid #767676;background:#efefef;padding:2px 10px;height:22px;color:#000;";
        } else {  // text/search/email/password/url/number...
            int size = 20; if (auto a = tree_->get_attribute(n, u"size")) { int v = std::atoi(narrow(*a).c_str()); if (v > 0) size = v; }
            st += "border:1px solid #767676;background:#fff;padding:2px 4px;height:22px;color:#000;width:" + std::to_string(size * 8 + 8) + "px;";
        }
        tree_->set_attribute(n, u"style", widen(st));
        // value / placeholder / button label as a text child.
        std::u16string txt = tree_->get_attribute(n, u"value").value_or(u"");
        bool placeholder = false;
        if (txt.empty()) { if (auto p = tree_->get_attribute(n, u"placeholder")) { txt = *p; placeholder = true; } }
        if (!txt.empty() && type != u"checkbox" && type != u"radio" && doc_->core(n) && doc_->core(n)->children.empty()) {
            if (placeholder) tree_->set_attribute(n, u"style", widen(st + "color:#757575;"));
            tree_->append_child(n, tree_->create_text_node(txt));
        }
    }

    // <select>: a native popup control shows only the selected option as a
    // single-line label (the option list is not in flow — UA CSS hides it).
    // Inject the selected (or first) option's text plus a dropdown caret.
    std::vector<malibu::NodeHandle> selects;
    tree_->query_selector_all(doc_->root(), u"select", selects);
    for (malibu::NodeHandle n : selects) {
        std::vector<malibu::NodeHandle> opts;
        tree_->query_selector_all(n, u"option", opts);
        // NB: a default-constructed NodeHandle is {0,0} = the document node, NOT
        // null (is_null() tests index==UINT32_MAX) — must seed with null_handle().
        malibu::NodeHandle chosen = malibu::NodeHandle::null_handle();
        for (malibu::NodeHandle o : opts) if (tree_->get_attribute(o, u"selected")) { chosen = o; break; }
        if (chosen.is_null() && !opts.empty()) chosen = opts[0];
        std::u16string label = chosen.is_null() ? u"" : tree_->text_content(chosen);
        // Collapse runs of whitespace so multi-line option text stays one line.
        std::u16string clean; bool sp = false;
        for (char16_t c : label) {
            bool ws = (c == u' ' || c == u'\t' || c == u'\n' || c == u'\r');
            if (ws) { if (!clean.empty()) sp = true; } else { if (sp) clean.push_back(u' '); sp = false; clean.push_back(c); }
        }
        std::string st;
        if (auto s = tree_->get_attribute(n, u"style")) st = narrow(*s) + ";";
        st += "display:inline-block;box-sizing:border-box;border:1px solid #767676;background:#fff;"
              "padding:2px 20px 2px 6px;height:22px;color:#000;white-space:nowrap;overflow:hidden;";
        tree_->set_attribute(n, u"style", widen(st));
        tree_->append_child(n, tree_->create_text_node(clean + u" ▾"));
    }
}

void View::apply_styles() {
    css::CSSParser cssp;
    // Rebuild the resolver from scratch (cheap) so re-styling after script
    // mutations is correct.
    resolver_ = std::make_unique<css::StyleResolver>();
    resolver_->add_stylesheet(cssp.parse(css::user_agent_css()), css::Origin::UserAgent);
    for (const std::u16string& sheet : pending_stylesheets_)
        resolver_->add_stylesheet(cssp.parse(sheet), css::Origin::Author);
    resolver_->set_viewport(static_cast<float>(last_vw_), static_cast<float>(last_vh_));  // @media
    resolver_->resolve(*doc_);
}

std::string View::resolve_url(const std::u16string& ref16) const {
    std::string ref = narrow(ref16);
    // Trim leading/trailing ASCII whitespace (srcset/href can carry it).
    size_t a = ref.find_first_not_of(" \t\r\n"); size_t b = ref.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return current_url_;
    ref = ref.substr(a, b - a + 1);
    // Protocol-relative (//host/path): adopt the base URL's scheme.
    if (ref.rfind("//", 0) == 0) {
        std::string scheme = current_url_.rfind("http://", 0) == 0 ? "http:" : "https:";
        return scheme + ref;
    }
    // Absolute URL (scheme://) — keep as-is.
    auto scheme = ref.find("://");
    if (scheme != std::string::npos && scheme < 12) return ref;
    // Root-relative (/foo): replace the path of the base URL's origin.
    if (ref[0] == '/') {
        auto s = current_url_.find("://");
        if (s != std::string::npos) {
            auto slash = current_url_.find('/', s + 3);
            std::string origin = slash == std::string::npos ? current_url_ : current_url_.substr(0, slash);
            return origin + ref;
        }
        return ref;  // e.g. file path root
    }
    // Document-relative: strip the base's last path segment.
    auto slash = current_url_.find_last_of('/');
    std::string dir = slash == std::string::npos ? current_url_ : current_url_.substr(0, slash + 1);
    return dir + ref;
}

void View::run_scripts(const std::vector<malibu::html::ScriptItem>& items) {
    for (const auto& it : items) {
        if (it.external) {
            if (!request_handler_) continue;  // no transport wired: skip external script
            std::string url = resolve_url(it.src);
            network::FetchResponse resp;
            if (!request_handler_(url, resp)) continue;  // 404 / unresolved
            std::string body(resp.body.begin(), resp.body.end());
            engine_.evaluate(body, url);
        } else {
            engine_.evaluate(narrow(it.code), current_url_);
        }
    }
    engine_.run_event_loop();  // settle promises / async / timers queued at load
}

void View::fire_window_event(const std::u16string& type) {
    using js::runtime::Value;
    using js::runtime::JSObject;
    using js::runtime::JSArray;
    auto& interp = engine_.interpreter();
    Value* we = interp.global()->find(u"__winEvents");
    if (!we) return;
    Value arrv = interp.get_prop_public(*we, type);
    if (!arrv.is_heap_ptr() || arrv.as_heap_ptr()->kind != js::vm::HeapObject::kJSArray) return;
    JSObject* ev = interp.new_object();
    ev->set(u"type", interp.str(narrow(type)));
    ev->set(u"target", *we);
    // Snapshot listeners so mutation during dispatch is safe.
    std::vector<Value> cbs = static_cast<JSArray*>(arrv.as_heap_ptr())->elements;
    for (Value cb : cbs) {
        if (!interp.is_callable(cb)) continue;
        std::vector<Value> args{ Value::make_heap_ptr(ev) };
        try { interp.call(cb, Value::make_undefined(), args); }
        catch (js::runtime::ThrowSignal&) {}
    }
    interp.run_microtasks();
}

void View::do_load(const std::string& html, const std::string& base_url) {
    layout_dirty_ = true;  // a fresh document always needs styling + layout
    // Set the URL/origin BEFORE reset_document so install_view_globals builds
    // `location` from the page being loaded (not the previous one).
    current_url_ = base_url;
    origin_ = security::Origin::parse(base_url);
    reset_document();

    html::HTMLParser parser;
    auto parsed = parser.parse(widen(html), *tree_);

    // External stylesheets (<link rel=stylesheet>) fetched via the host, then
    // inline <style> blocks — author order so later rules win.
    pending_stylesheets_.clear();
    if (request_handler_) {
        for (const std::u16string& href : parsed.external_styles) {
            network::FetchResponse resp;
            if (request_handler_(resolve_url(href), resp))
                pending_stylesheets_.push_back(widen(std::string(resp.body.begin(), resp.body.end())));
        }
    }
    for (auto& s : parsed.stylesheets) pending_stylesheets_.push_back(s);

    load_images(parsed);            // fetch + decode <img> (sizes boxes for layout)
    apply_styles();                 // initial cascade
    run_scripts(parsed.script_items);  // inline + external scripts, in document order
    fire_window_event(u"DOMContentLoaded");
    fire_window_event(u"load");
    engine_.run_event_loop();       // settle anything queued by load handlers
    apply_styles();                 // re-style after script mutations
}

bool View::load_html(const std::string& html, const std::string& base_url) {
    do_load(html, base_url);
    // Push a new history entry, dropping any forward entries.
    history_.resize(history_.empty() ? 0 : history_pos_ + 1);
    history_html_.resize(history_.size());
    history_.push_back(base_url);
    history_html_.push_back(html);
    history_pos_ = history_.size() - 1;
    return true;
}

bool View::load_file(const std::string& path) {
    if (sandbox_ & SandboxNoNavigation) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    return load_html(ss.str(), "file://" + path);
}

bool View::load_url(const std::string& url) {
    if (sandbox_ & SandboxNoNavigation) return false;
    // Request interception lets the host satisfy the navigation locally.
    if (request_handler_) {
        network::FetchResponse resp;
        if (request_handler_(url, resp)) {
            return load_html(std::string(resp.body.begin(), resp.body.end()), url);
        }
    }
    if (sandbox_ & SandboxNoNetwork) return false;
    // No transport is wired into the view by default; embedders intercept via
    // set_request_handler. (The FetchEngine is exercised separately.)
    return false;
}

void View::reload() {
    if (history_pos_ < history_html_.size())
        do_load(history_html_[history_pos_], history_[history_pos_]);
}

bool View::go_back() {
    if (history_pos_ == 0 || history_.empty()) return false;
    --history_pos_;
    do_load(history_html_[history_pos_], history_[history_pos_]);
    return true;
}

bool View::go_forward() {
    if (history_pos_ + 1 >= history_.size()) return false;
    ++history_pos_;
    do_load(history_html_[history_pos_], history_[history_pos_]);
    return true;
}

bool View::dispatch_event(const std::string& selector, const std::string& type, bool bubbles) {
    if (!binding_) return false;
    malibu::NodeHandle node = tree_->query_selector(doc_->root(), widen(selector));
    if (node.is_null()) return false;
    layout_dirty_ = true;  // a JS handler may mutate the DOM
    return binding_->dispatch_event(node, widen(type), bubbles, /*cancelable=*/true);
}

std::string View::eval_js(const std::string& source) {
    layout_dirty_ = true;  // evaluated JS may mutate the DOM/styles
    auto r = engine_.evaluate(source, current_url_);
    if (!r.ok) return "error: " + r.error;
    return narrow(engine_.interpreter().json_stringify(r.value));
}

render::Framebuffer View::render(int width, int height, float scroll_y) {
    // Restyle+relayout only when content or viewport changed. A pure scroll keeps
    // the cached layout tree and just re-rasterizes at the new offset — this is the
    // difference between re-parsing all CSS + relaying-out every wheel tick (very
    // slow) and an instant scroll.
    bool relayout = layout_dirty_ || width != last_vw_ || height != last_vh_ || layout_.root() == nullptr;
    last_vw_ = width; last_vh_ = height;
    layout::LayoutBox* root;
    if (relayout) {
        apply_styles();
        root = layout_.layout_document(*doc_, static_cast<float>(width), static_cast<float>(height));
        layout_dirty_ = false;
    } else {
        root = layout_.root();
    }
    render::Framebuffer fb = renderer_.render(*doc_, root, width, height, {255, 255, 255, 255},
                                              drawer_.get(), scroll_y);
    composite_canvases(fb, scroll_y);  // blit <canvas>/<img> bitmaps over the page
    return fb;
}

malibu::NodeHandle View::node_at(float x, float y) {
    layout::LayoutBox* b = layout_.hit_test(x, y);
    return b ? b->node : malibu::NodeHandle::null_handle();
}

bool View::set_hover(float x, float y) {
    layout::LayoutBox* b = layout_.hit_test(x, y);
    malibu::NodeHandle hit = b ? b->node : malibu::NodeHandle::null_handle();
    if (hit == hovered_) return false;
    // Clear the old hover chain, set the new one (ancestors get :hover too).
    for (malibu::NodeHandle n = hovered_; doc_->core(n); n = doc_->core(n)->parent) doc_->core(n)->hovered = false;
    hovered_ = hit;
    for (malibu::NodeHandle n = hovered_; doc_->core(n); n = doc_->core(n)->parent) doc_->core(n)->hovered = true;
    layout_dirty_ = true;  // :hover changed → restyle
    return true;
}

malibu::NodeHandle View::dispatch_mouse(float x, float y, const std::string& type, int button) {
    (void)button;
    malibu::NodeHandle hit = node_at(x, y);
    if (!doc_->core(hit)) return hit;
    layout_dirty_ = true;  // :active/:focus change + JS handlers may mutate the DOM
    if (type == "mousedown") doc_->core(hit)->active = true;
    if (type == "click") {                       // move focus to the clicked element
        if (doc_->core(focused_)) doc_->core(focused_)->focused = false;
        focused_ = hit;
        doc_->core(focused_)->focused = true;
    }
    binding_->dispatch_event(hit, widen(type), /*bubbles=*/true, /*cancelable=*/true);
    engine_.run_event_loop();
    if ((type == "mouseup" || type == "click") && doc_->core(hit)) doc_->core(hit)->active = false;
    return hit;
}

void View::dispatch_key(const std::string& key, bool is_text) {
    auto* c = doc_->core(focused_);
    if (!c || (c->tag_name != u"input" && c->tag_name != u"textarea")) return;
    layout_dirty_ = true;  // editing the value + input/change handlers
    std::u16string val = tree_->get_attribute(focused_, u"value").value_or(u"");
    if (is_text) val += widen(key);
    else if (key == "Backspace") { if (!val.empty()) val.pop_back(); }
    else if (key == "Enter") { binding_->dispatch_event(focused_, u"change", true, true); engine_.run_event_loop(); return; }
    else return;
    tree_->set_attribute(focused_, u"value", val);
    binding_->dispatch_event(focused_, u"input", true, true);
    engine_.run_event_loop();
}

float View::page_height(int width) {
    last_vw_ = width;
    apply_styles();
    layout::LayoutBox* root = layout_.layout_document(*doc_, static_cast<float>(width), 800.0f);
    return root ? root->height : 0.0f;
}

void View::post_message(const std::string& message) {
    auto& interp = engine_.interpreter();
    js::runtime::Value* gt = interp.global()->find(u"globalThis");
    if (!gt) return;
    js::runtime::Value handler = interp.get_prop_public(*gt, u"__malibuReceiveMessage");
    if (interp.is_callable(handler)) {
        std::vector<js::runtime::Value> args{interp.str(message)};
        try { interp.call(handler, js::runtime::Value::make_undefined(), args); }
        catch (js::runtime::ThrowSignal&) {}
        interp.run_microtasks();
    }
}

} // namespace malibu::view
