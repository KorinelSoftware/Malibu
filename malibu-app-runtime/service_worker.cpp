// malibu-app-runtime/service_worker.cpp
// Service Worker realm: self/addEventListener, caches (Cache API), Response,
// install/activate lifecycle, and fetch interception via event.respondWith.

#include "malibu/app/service_worker.h"

namespace malibu::app {

using js::runtime::Value;
using js::runtime::Interpreter;

namespace {
std::string narrow(const std::u16string& s) { std::string r; for (char16_t c : s) r.push_back(static_cast<char>(c & 0xFF)); return r; }
}  // namespace

ServiceWorkerHost::ServiceWorkerHost(storage::CacheStorage& caches) : caches_(&caches) {
    respond_value_ = Value::make_undefined();
    install_globals();
}

Value ServiceWorkerHost::make_response(const std::string& body, int status) {
    auto& in = engine_.interpreter();
    auto* obj = in.new_object();
    Value v = Value::make_heap_ptr(obj);
    in.set_prop_public(v, u"body", in.str(body));
    in.set_prop_public(v, u"status", Value::make_int32(status));
    in.set_prop_public(v, u"ok", Value::make_bool(status >= 200 && status < 300));
    in.set_prop_public(v, u"__isResponse", Value::make_bool(true));
    return v;
}

Value ServiceWorkerHost::resolved_promise(Value v) {
    auto& in = engine_.interpreter();
    auto* p = in.new_promise();
    in.resolve_promise(p, v);
    return Value::make_heap_ptr(p);
}

void ServiceWorkerHost::install_globals() {
    auto& in = engine_.interpreter();
    ServiceWorkerHost* self = this;

    // addEventListener(type, callback)
    auto* add = in.new_native(u"addEventListener",
        [self](Interpreter& i, Value, std::vector<Value>& a) -> Value {
            if (a.size() >= 2 && i.is_callable(a[1])) {
                self->listeners_[narrow(i.to_string(a[0]))].push_back(a[1]);
                i.add_host_root(a[1]);  // keep the callback alive for the SW lifetime
            }
            return Value::make_undefined();
        });
    in.global()->define(u"addEventListener", Value::make_heap_ptr(add));
    if (Value* gt = in.global()->find(u"globalThis")) {
        in.set_prop_public(*gt, u"addEventListener", Value::make_heap_ptr(add));
        in.global()->define(u"self", *gt);
    }

    // Response constructor: new Response(body, init)
    auto* response_ctor = in.new_native(u"Response",
        [self](Interpreter& i, Value, std::vector<Value>& a) -> Value {
            std::string body = a.empty() ? std::string() : narrow(i.to_string(a[0]));
            int status = 200;
            if (a.size() > 1 && a[1].is_heap_ptr()) {
                Value s = i.get_prop_public(a[1], u"status");
                if (s.is_int32() || s.is_double()) status = static_cast<int>(i.to_number(s));
            }
            return self->make_response(body, status);
        });
    in.global()->define(u"Response", Value::make_heap_ptr(response_ctor));

    // caches.open(name) -> Promise<Cache>
    auto* caches_obj = in.new_object();
    Value caches_val = Value::make_heap_ptr(caches_obj);
    auto* open_fn = in.new_native(u"open",
        [self](Interpreter& i, Value, std::vector<Value>& a) -> Value {
            std::string name = a.empty() ? std::string() : narrow(i.to_string(a[0]));
            self->caches_->open(name);
            auto* cache = i.new_object();
            Value cv = Value::make_heap_ptr(cache);
            i.set_prop_public(cv, u"_name", i.str(name));

            auto* match_fn = i.new_native(u"match",
                [self](Interpreter& i2, Value thisv, std::vector<Value>& ma) -> Value {
                    std::string cn = narrow(i2.to_string(i2.get_prop_public(thisv, u"_name")));
                    std::string url = ma.empty() ? std::string() : narrow(i2.to_string(ma[0]));
                    auto m = self->caches_->match(cn, url);
                    if (m) return self->resolved_promise(
                        self->make_response(std::string(m->body.begin(), m->body.end()), m->status));
                    return self->resolved_promise(Value::make_undefined());
                });
            auto* put_fn = i.new_native(u"put",
                [self](Interpreter& i2, Value thisv, std::vector<Value>& pa) -> Value {
                    std::string cn = narrow(i2.to_string(i2.get_prop_public(thisv, u"_name")));
                    std::string url = pa.empty() ? std::string() : narrow(i2.to_string(pa[0]));
                    storage::CacheStorage::CachedResponse cr;
                    if (pa.size() > 1) {
                        std::string body = narrow(i2.to_string(i2.get_prop_public(pa[1], u"body")));
                        cr.body.assign(body.begin(), body.end());
                        Value st = i2.get_prop_public(pa[1], u"status");
                        cr.status = (st.is_int32() || st.is_double()) ? static_cast<int>(i2.to_number(st)) : 200;
                    }
                    self->caches_->put(cn, url, std::move(cr));
                    return self->resolved_promise(Value::make_undefined());
                });
            i.set_prop_public(cv, u"match", Value::make_heap_ptr(match_fn));
            i.set_prop_public(cv, u"put", Value::make_heap_ptr(put_fn));
            return self->resolved_promise(cv);
        });
    in.set_prop_public(caches_val, u"open", Value::make_heap_ptr(open_fn));
    in.global()->define(u"caches", caches_val);

    // fetch(url): no network in the SW sandbox by default → rejected promise.
    auto* fetch_fn = in.new_native(u"fetch",
        [self](Interpreter& i, Value, std::vector<Value>&) -> Value {
            auto* p = i.new_promise();
            i.reject_promise(p, i.str(std::string("TypeError: network unavailable in service worker")));
            return Value::make_heap_ptr(p);
        });
    in.global()->define(u"fetch", Value::make_heap_ptr(fetch_fn));
}

bool ServiceWorkerHost::register_script(const std::string& source) {
    auto r = engine_.evaluate(source, "service-worker.js");
    if (!r.ok) { state_ = State::Failed; error_ = r.error; return false; }
    return true;
}

void ServiceWorkerHost::dispatch_event(const std::string& type, Value event) {
    auto& in = engine_.interpreter();
    in.push_root(event);
    auto it = listeners_.find(type);
    if (it != listeners_.end()) {
        for (Value cb : it->second) {
            std::vector<Value> args{event};
            try { in.call(cb, Value::make_undefined(), args); }
            catch (js::runtime::ThrowSignal&) {}
        }
    }
    in.run_microtasks();
    in.pop_root();
}

void ServiceWorkerHost::install() {
    auto& in = engine_.interpreter();
    state_ = State::Installing;
    waituntil_.clear();

    auto* ev = in.new_object();
    Value event = Value::make_heap_ptr(ev);
    ServiceWorkerHost* self = this;
    auto* wait = in.new_native(u"waitUntil",
        [self](Interpreter& i, Value, std::vector<Value>& a) -> Value {
            if (!a.empty()) { self->waituntil_.push_back(a[0]); i.add_host_root(a[0]); }
            return Value::make_undefined();
        });
    in.set_prop_public(event, u"waitUntil", Value::make_heap_ptr(wait));

    dispatch_event("install", event);
    for (Value p : waituntil_) { try { in.await_settled(p); } catch (js::runtime::ThrowSignal&) {} in.remove_host_root(p); }
    waituntil_.clear();
    state_ = State::Installed;
}

void ServiceWorkerHost::activate() {
    auto& in = engine_.interpreter();
    state_ = State::Activating;
    waituntil_.clear();

    auto* ev = in.new_object();
    Value event = Value::make_heap_ptr(ev);
    ServiceWorkerHost* self = this;
    auto* wait = in.new_native(u"waitUntil",
        [self](Interpreter& i, Value, std::vector<Value>& a) -> Value {
            if (!a.empty()) { self->waituntil_.push_back(a[0]); i.add_host_root(a[0]); }
            return Value::make_undefined();
        });
    in.set_prop_public(event, u"waitUntil", Value::make_heap_ptr(wait));

    dispatch_event("activate", event);
    for (Value p : waituntil_) { try { in.await_settled(p); } catch (js::runtime::ThrowSignal&) {} in.remove_host_root(p); }
    waituntil_.clear();
    state_ = State::Activated;
}

std::optional<ServiceWorkerHost::Response> ServiceWorkerHost::handle_fetch(const std::string& url) {
    if (state_ != State::Activated) return std::nullopt;
    auto& in = engine_.interpreter();

    responded_ = false;
    respond_value_ = Value::make_undefined();

    auto* ev = in.new_object();
    Value event = Value::make_heap_ptr(ev);
    in.set_prop_public(event, u"request", in.str(url));
    ServiceWorkerHost* self = this;
    auto* respond = in.new_native(u"respondWith",
        [self](Interpreter&, Value, std::vector<Value>& a) -> Value {
            self->respond_value_ = a.empty() ? Value::make_undefined() : a[0];
            self->responded_ = true;
            return Value::make_undefined();
        });
    in.set_prop_public(event, u"respondWith", Value::make_heap_ptr(respond));

    in.push_root(event);
    auto it = listeners_.find("fetch");
    if (it != listeners_.end()) {
        for (Value cb : it->second) {
            std::vector<Value> args{event};
            try { in.call(cb, Value::make_undefined(), args); }
            catch (js::runtime::ThrowSignal&) {}
        }
    }
    in.pop_root();

    if (!responded_) { in.run_microtasks(); return std::nullopt; }

    in.push_root(respond_value_);
    Value v;
    try { v = in.await_settled(respond_value_); }
    catch (js::runtime::ThrowSignal&) { in.pop_root(); return std::nullopt; }
    in.pop_root();

    Response resp;
    Value body = in.get_prop_public(v, u"body");
    if (!body.is_undefined()) {
        resp.body = narrow(in.to_string(body));
        Value st = in.get_prop_public(v, u"status");
        resp.status = (st.is_int32() || st.is_double()) ? static_cast<int>(in.to_number(st)) : 200;
    } else {
        resp.body = narrow(in.to_string(v));
    }
    return resp;
}

} // namespace malibu::app
