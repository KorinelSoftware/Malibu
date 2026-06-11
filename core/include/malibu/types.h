#pragma once
// core/include/malibu/types.h
// Forward declarations and fundamental type aliases for Malibu Engine.
// This header must remain free of OS-specific includes.

#include <cstdint>

namespace malibu {

// ---------------------------------------------------------------------------
// NodeHandle — identity token for a DOM node in the NodeTable slab.
//
// Uniqueness is guaranteed by the (index, generation) pair: when a slot is
// reused after free, its generation counter is incremented by exactly 1.
// Stale handles (old generation) are detectable at O(1) via NodeTable::resolve.
// ---------------------------------------------------------------------------
struct NodeHandle {
    uint32_t index;
    uint32_t generation;

    bool operator==(const NodeHandle&) const noexcept = default;
    bool operator!=(const NodeHandle&) const noexcept = default;

    /// Returns the canonical null/invalid sentinel value.
    static constexpr NodeHandle null_handle() noexcept {
        return {UINT32_MAX, 0};
    }

    /// Returns true when this handle is the null sentinel.
    [[nodiscard]] constexpr bool is_null() const noexcept {
        return index == UINT32_MAX;
    }
};

// ---------------------------------------------------------------------------
// RealmId — opaque monotonic identifier assigned to each JSRealm instance.
// ---------------------------------------------------------------------------
using RealmId = uint32_t;

/// Sentinel value indicating an invalid / unassigned realm.
inline constexpr RealmId kInvalidRealmId = UINT32_MAX;

// ---------------------------------------------------------------------------
// JSObjectHandle — opaque reference to a JS object in the MalibuJS heap.
// The full definition lives in js/vm/; this forward alias is enough for
// headers that need to forward-declare cross-subsystem relationships.
// ---------------------------------------------------------------------------
struct JSObjectHandle {
    uint64_t raw{0};

    [[nodiscard]] bool is_null() const noexcept { return raw == 0; }

    bool operator==(const JSObjectHandle&) const noexcept = default;
    bool operator!=(const JSObjectHandle&) const noexcept = default;
};

// ---------------------------------------------------------------------------
// Forward declarations — concrete definitions in their respective subsystems.
// ---------------------------------------------------------------------------
// NodeCore is defined in dom/node_table.h (not forward declared to avoid class/struct mismatch)
class  NodeTable;           // dom/node_table.h
class  WrapperCache;        // dom/wrapper_cache.h
class  DomCommandBuffer;    // dom/dom_command_buffer.h
class  DOMTree;             // dom/document.h
class  JSRealm;             // js/vm/realm.h
class  UnifiedObjectGraph;  // unified_object_graph/uog.h

namespace css    { struct ComputedStyle; }  // css/computed_style/computed_style.h
namespace layout { struct LayoutBox; }       // layout/layout_box.h

} // namespace malibu
