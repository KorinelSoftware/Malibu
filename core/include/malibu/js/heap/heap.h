#pragma once
// core/include/malibu/js/heap/heap.h
// Precise mark-and-sweep heap for MalibuJS runtime objects (Task 18, baseline).
//
// The heap owns every runtime HeapObject. A collection marks from a root set
// (supplied by the interpreter) and sweeps the rest. Collection runs only at
// interpreter-safe points, so all live values are reachable from the roots.
// (Incremental/tri-color stepping is a future optimisation; this stop-the-world
// collector is correct and exact.)

#include <cstddef>
#include <functional>
#include <vector>

#include "malibu/js/vm/value.h"

namespace malibu::js::heap {

using vm::Value;
using vm::HeapObject;

class Heap {
public:
    Heap() = default;
    ~Heap();
    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    // Allocates and tracks a runtime object. Never triggers a collection.
    template <class T, class... Args>
    T* alloc(Args&&... args) {
        T* obj = new T(std::forward<Args>(args)...);
        objects_.push_back(obj);
        bytes_since_gc_ += sizeof(T);
        return obj;
    }

    // The interpreter installs a callback that marks all of its roots. The
    // callback receives a mark function it must call on every root Value.
    using MarkFn = std::function<void(Value)>;
    void set_root_enumerator(std::function<void(const MarkFn&)> fn) {
        root_enumerator_ = std::move(fn);
    }

    // Marks from roots, then sweeps unreachable objects.
    void collect();

    // Heuristic: should the interpreter collect at the next safe point?
    [[nodiscard]] bool should_collect() const noexcept { return bytes_since_gc_ >= threshold_; }
    void set_threshold(size_t bytes) noexcept { threshold_ = bytes; }

    [[nodiscard]] size_t live_count() const noexcept { return objects_.size(); }

private:
    void mark_value(Value v, std::vector<HeapObject*>& worklist);
    void trace(HeapObject* obj, std::vector<HeapObject*>& worklist);

    std::vector<HeapObject*>                 objects_;
    std::function<void(const MarkFn&)>       root_enumerator_;
    size_t                                   bytes_since_gc_ = 0;
    size_t                                   threshold_      = 4 * 1024 * 1024;  // 4 MB
};

} // namespace malibu::js::heap
