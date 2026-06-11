// unified_object_graph/uog.cpp
// Unified Object Graph implementation.

#include "malibu/unified_object_graph/uog.h"
#include "malibu/diagnostics/diagnostic_log.h"
#include <sstream>

namespace malibu::unified_object_graph {

size_t NodeHandleHash::operator()(const malibu::NodeHandle& h) const noexcept {
    uint64_t v = (static_cast<uint64_t>(h.index) << 32) | h.generation;
    // SplitMix64 finalizer
    v ^= v >> 30;
    v *= 0xbf58476d1ce4e5b9ULL;
    v ^= v >> 27;
    v *= 0x94d049bb133111ebULL;
    v ^= v >> 31;
    return static_cast<size_t>(v);
}

void UnifiedObjectGraph::on_wrapper_created(malibu::NodeHandle dom_node, malibu::JSObjectHandle wrapper) {
    std::lock_guard lock(mu_);
    auto& entry = entries_[dom_node];
    entry.wrapper = wrapper;
    entry.is_gc_root = false;  // wrapper now keeps it alive
}

void UnifiedObjectGraph::on_listener_registered(malibu::NodeHandle dom_node, malibu::JSObjectHandle fn) {
    std::lock_guard lock(mu_);
    auto& entry = entries_[dom_node];
    entry.listeners.push_back(fn);
    if (!entry.is_gc_root && entry.wrapper.is_null()) {
        entry.is_gc_root = true;  // listener keeps node alive
    }
}

void UnifiedObjectGraph::on_listener_removed(malibu::NodeHandle dom_node, malibu::JSObjectHandle fn) {
    std::lock_guard lock(mu_);
    auto it = entries_.find(dom_node);
    if (it != entries_.end()) {
        auto& listeners = it->second.listeners;
        // Use std::erase (C++20) to avoid name conflict with std::remove
        std::erase(listeners, fn);
        
        // If no more listeners and no wrapper, can be collected if disconnected
        if (listeners.empty() && it->second.wrapper.is_null()) {
            // Don't clear is_gc_root here - node might still be connected
            // GC will handle it via enumerate_gc_roots
        }
    }
}

void UnifiedObjectGraph::on_wrapper_collected(malibu::NodeHandle dom_node) {
    std::lock_guard lock(mu_);
    auto it = entries_.find(dom_node);
    if (it != entries_.end()) {
        it->second.wrapper = malibu::JSObjectHandle{};  // null
        // Re-register as GC root if node is still alive (connected/detached with listeners)
        // The GC will check this via enumerate_gc_roots
        // We keep is_gc_root as-is; the GC's root enumeration will determine liveness
    }
}

void UnifiedObjectGraph::on_node_destroyed(malibu::NodeHandle dom_node) {
    std::lock_guard lock(mu_);
    entries_.erase(dom_node);
}

template<typename GCRootVisitor>
void UnifiedObjectGraph::enumerate_gc_roots(GCRootVisitor& visitor) {
    std::lock_guard lock(mu_);
    for (const auto& [handle, entry] : entries_) {
        // Node is a GC root if:
        // 1. No wrapper exists (wrapper is null)
        // 2. Node is still alive (has listeners or is connected)
        if (entry.wrapper.is_null()) {
            if (entry.is_gc_root || !entry.listeners.empty()) {
                // The visitor should add the node handle itself as a root
                // For DOM nodes, we visit the NodeCore* or equivalent
                // This is a placeholder - actual implementation depends on GC design
            }
        }
    }
}

template<typename GCTracer>
void UnifiedObjectGraph::trace_references(GCTracer& tracer) {
    std::lock_guard lock(mu_);
    for (const auto& [handle, entry] : entries_) {
        // Trace from JS wrapper to DOM node
        if (!entry.wrapper.is_null()) {
            // tracer.trace(entry.wrapper, handle); // depends on tracer API
        }
        // Trace from DOM node to JS event listeners
        for (const auto& listener : entry.listeners) {
            if (!listener.is_null()) {
                // tracer.trace(handle, listener); // depends on tracer API
            }
        }
    }
}

// Explicit template instantiations removed - will be added when GC visitor types are defined

} // namespace malibu::unified_object_graph