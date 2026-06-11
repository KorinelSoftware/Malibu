#pragma once
// core/include/malibu/unified_object_graph/uog.h
// Unified Object Graph - cross-heap reference tracking.

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <mutex>
#include "../types.h"

namespace malibu::unified_object_graph {

struct NodeHandleHash {
    size_t operator()(const malibu::NodeHandle& h) const noexcept;
};

class UnifiedObjectGraph {
public:
    struct NodeEntry {
        malibu::JSObjectHandle wrapper;
        bool is_gc_root = false;
        std::vector<malibu::JSObjectHandle> listeners;
    };
    
    void on_wrapper_created(malibu::NodeHandle dom_node, malibu::JSObjectHandle wrapper);
    void on_listener_registered(malibu::NodeHandle dom_node, malibu::JSObjectHandle fn);
    void on_listener_removed(malibu::NodeHandle dom_node, malibu::JSObjectHandle fn);
    void on_wrapper_collected(malibu::NodeHandle dom_node);
    void on_node_destroyed(malibu::NodeHandle dom_node);
    
    template<typename GCRootVisitor>
    void enumerate_gc_roots(GCRootVisitor& visitor);
    
    template<typename GCTracer>
    void trace_references(GCTracer& tracer);
private:
    std::unordered_map<malibu::NodeHandle, NodeEntry, NodeHandleHash> entries_;
    std::mutex mu_;
};

} // namespace malibu::unified_object_graph