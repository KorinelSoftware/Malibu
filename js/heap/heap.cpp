// js/heap/heap.cpp
// Mark-and-sweep collection over the runtime object graph.

#include "malibu/js/heap/heap.h"
#include "malibu/js/runtime/objects.h"

namespace malibu::js::heap {

using namespace malibu::js::runtime;

Heap::~Heap() {
    for (HeapObject* o : objects_) delete o;
}

void Heap::mark_value(Value v, std::vector<HeapObject*>& worklist) {
    if (!v.is_heap_ptr()) return;
    HeapObject* o = v.as_heap_ptr();
    if (!o || o->gc_color == vm::GCColor::Black) return;
    o->gc_color = vm::GCColor::Black;
    worklist.push_back(o);
}

void Heap::trace(HeapObject* obj, std::vector<HeapObject*>& worklist) {
    auto mark_prop = [this](const Property& p, std::vector<HeapObject*>& wl) {
        mark_value(p.value, wl);
        if (p.is_accessor) { mark_value(p.getter, wl); mark_value(p.setter, wl); }
    };
    switch (obj->kind) {
        case HeapObject::kJSString:
            break;  // no outgoing JS-heap edges
        case HeapObject::kDomNodeRef: {
            auto* d = static_cast<malibu::js::vm::DomNodeRef*>(obj);
            for (auto& [k, raw] : d->expandos) { (void)k; Value v; v.raw = raw; mark_value(v, worklist); }
            break;
        }
        case HeapObject::kJSArray: {
            auto* a = static_cast<JSArray*>(obj);
            for (Value e : a->elements) mark_value(e, worklist);
            for (auto& p : a->props) mark_prop(p, worklist);
            if (a->proto) mark_value(Value::make_heap_ptr(a->proto), worklist);
            break;
        }
        case HeapObject::kJSObject: {
            auto* o = static_cast<JSObject*>(obj);
            for (auto& p : o->props) mark_prop(p, worklist);
            if (o->proto) mark_value(Value::make_heap_ptr(o->proto), worklist);
            break;
        }
        case HeapObject::kJSPromise: {
            auto* p = static_cast<JSPromise*>(obj);
            for (auto& pr : p->props) mark_prop(pr, worklist);
            if (p->proto) mark_value(Value::make_heap_ptr(p->proto), worklist);
            mark_value(p->result, worklist);
            for (auto& r : p->reactions) {
                mark_value(r.on_fulfilled, worklist);
                mark_value(r.on_rejected, worklist);
                if (r.result) mark_value(Value::make_heap_ptr(r.result), worklist);
            }
            break;
        }
        case HeapObject::kJSFunction: {
            auto* f = static_cast<JSFunction*>(obj);
            if (f->closure) mark_value(Value::make_heap_ptr(f->closure), worklist);
            for (auto& p : f->props) mark_value(p.value, worklist);
            break;
        }
        case HeapObject::kJSMap: {
            auto* m = static_cast<JSMap*>(obj);
            for (auto& [k, v] : m->entries) { mark_value(k, worklist); mark_value(v, worklist); }
            for (auto& p : m->props) mark_prop(p, worklist);
            if (m->proto) mark_value(Value::make_heap_ptr(m->proto), worklist);
            break;
        }
        case HeapObject::kJSSet: {
            auto* s = static_cast<JSSet*>(obj);
            for (Value v : s->items) mark_value(v, worklist);
            for (auto& p : s->props) mark_prop(p, worklist);
            if (s->proto) mark_value(Value::make_heap_ptr(s->proto), worklist);
            break;
        }
        case HeapObject::kJSGenerator: {
            auto* g = static_cast<JSGenerator*>(obj);
            for (auto& p : g->props) mark_prop(p, worklist);
            if (g->proto) mark_value(Value::make_heap_ptr(g->proto), worklist);
            // The backing frame's Values are marked via Interpreter::gen_frames_.
            break;
        }
        case HeapObject::kArrayBuffer: {
            auto* ab = static_cast<JSArrayBuffer*>(obj);
            for (auto& p : ab->props) mark_prop(p, worklist);
            if (ab->proto) mark_value(Value::make_heap_ptr(ab->proto), worklist);
            break;  // (the byte store has no JS-heap edges)
        }
        case HeapObject::kTypedArray: {
            auto* ta = static_cast<JSTypedArray*>(obj);
            if (ta->buffer) mark_value(Value::make_heap_ptr(ta->buffer), worklist);
            for (auto& p : ta->props) mark_prop(p, worklist);
            if (ta->proto) mark_value(Value::make_heap_ptr(ta->proto), worklist);
            break;
        }
        case HeapObject::kDataView: {
            auto* dv = static_cast<JSDataView*>(obj);
            if (dv->buffer) mark_value(Value::make_heap_ptr(dv->buffer), worklist);
            for (auto& p : dv->props) mark_prop(p, worklist);
            if (dv->proto) mark_value(Value::make_heap_ptr(dv->proto), worklist);
            break;
        }
        case HeapObject::kJSProxy: {
            auto* px = static_cast<JSProxy*>(obj);
            mark_value(px->target, worklist);
            mark_value(px->handler, worklist);
            for (auto& p : px->props) mark_prop(p, worklist);
            if (px->proto) mark_value(Value::make_heap_ptr(px->proto), worklist);
            break;
        }
        case HeapObject::kEnvironment: {
            auto* e = static_cast<Environment*>(obj);
            for (auto& [k, v] : e->slots) mark_value(v, worklist);
            if (e->parent) mark_value(Value::make_heap_ptr(e->parent), worklist);
            if (e->object_backing) mark_value(Value::make_heap_ptr(e->object_backing), worklist);
            break;
        }
    }
}

void Heap::collect() {
    // 1. Reset colors.
    for (HeapObject* o : objects_) o->gc_color = vm::GCColor::White;

    // 2. Mark from roots.
    std::vector<HeapObject*> worklist;
    if (root_enumerator_) {
        root_enumerator_([&](Value v) { mark_value(v, worklist); });
    }
    while (!worklist.empty()) {
        HeapObject* o = worklist.back();
        worklist.pop_back();
        trace(o, worklist);
    }

    // 3. Sweep white.
    std::vector<HeapObject*> survivors;
    survivors.reserve(objects_.size());
    for (HeapObject* o : objects_) {
        if (o->gc_color == vm::GCColor::Black) survivors.push_back(o);
        else delete o;
    }
    objects_.swap(survivors);
    bytes_since_gc_ = 0;
}

} // namespace malibu::js::heap
