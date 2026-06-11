#pragma once
// core/include/malibu/js/vm/value.h
// NaN-boxed Value type and HeapObject hierarchy (Task 16).
//
// Layout (64-bit):
//   A non-NaN double is stored as its raw IEEE-754 bits (tag 0 — no boxing).
//   Every other value is a quiet NaN with a 3-bit tag in bits[50:48] and a
//   48-bit payload in bits[47:0]:
//       raw = 0x7FF8_0000_0000_0000 | (tag << 48) | payload
//   tag 1 = int32, 2 = bool, 3 = null, 4 = undefined, 5 = HeapPtr.
//   NaN doubles are canonicalised to 0x7FF8.. (tag 0) so they never collide
//   with a tagged value.

#include <unordered_map>
#include <string>
#include <cstdint>
#include "../../types.h"

namespace malibu::js::vm {

enum class GCColor : uint8_t { White = 0, Grey = 1, Black = 2 };

// Every GC-managed allocation begins with this header.
struct HeapObject {
    enum Kind : uint8_t {
        kJSObject     = 0,
        kJSString     = 1,
        kJSArray      = 2,
        kJSFunction   = 3,
        kDomNodeRef   = 4,
        kEnvironment  = 5,  // lexical scope record (runtime)
        kJSPromise    = 6,  // Promise object (runtime)
        kJSMap        = 7,  // Map object (runtime)
        kJSSet        = 8,  // Set object (runtime)
        kJSGenerator  = 9,  // Generator/iterator object (runtime)
        kArrayBuffer  = 10, // ArrayBuffer (raw byte store)
        kTypedArray   = 11, // Int8Array .. Float64Array (view over an ArrayBuffer)
        kDataView     = 12, // DataView (typed get/set over an ArrayBuffer)
        kJSProxy      = 13, // Proxy (target + handler with traps)
    };
    Kind     kind     = kJSObject;
    GCColor  gc_color = GCColor::White;
    uint16_t shape_id = 0;
    uint32_t flags    = 0;

    // Virtual so the GC can delete derived runtime objects (which own
    // std::string/std::vector members) through a HeapObject* base pointer.
    virtual ~HeapObject() = default;
};

// Heap-allocated DOM node reference. Stored as a Value with the HeapPtr tag.
struct DomNodeRef : HeapObject {
    malibu::NodeHandle     handle{};   // full 32+32 bits — no truncation
    malibu::JSObjectHandle wrapper{};  // null until first property access
    // Expando (JS-assigned) properties — DOM nodes are ordinary objects that can
    // hold arbitrary properties. Stored as raw Value bits (Value is defined below);
    // the GC traces any heap pointers among them.
    std::unordered_map<std::u16string, uint64_t> expandos;
    DomNodeRef() { kind = kDomNodeRef; }
};

struct Value {
    uint64_t raw = 0;

    static constexpr uint64_t kQuietNan = 0x7FF8000000000000ULL;
    static constexpr uint64_t kPayloadMask = 0x0000FFFFFFFFFFFFULL;  // 48 bits

    static Value make_double(double d) noexcept;
    static Value make_int32(int32_t v) noexcept;
    static Value make_bool(bool v) noexcept;
    static Value make_null() noexcept;
    static Value make_undefined() noexcept;
    static Value make_heap_ptr(HeapObject* p) noexcept;

    [[nodiscard]] bool        is_tagged()    const noexcept;
    [[nodiscard]] uint8_t     tag()          const noexcept;

    [[nodiscard]] bool        is_double()    const noexcept { return !is_tagged(); }
    [[nodiscard]] double      as_double()    const noexcept;
    [[nodiscard]] bool        is_int32()     const noexcept { return is_tagged() && tag() == 1; }
    [[nodiscard]] int32_t     as_int32()     const noexcept;
    [[nodiscard]] bool        is_bool()      const noexcept { return is_tagged() && tag() == 2; }
    [[nodiscard]] bool        as_bool()      const noexcept;
    [[nodiscard]] bool        is_null()      const noexcept { return is_tagged() && tag() == 3; }
    [[nodiscard]] bool        is_undefined() const noexcept { return is_tagged() && tag() == 4; }
    [[nodiscard]] bool        is_heap_ptr()  const noexcept { return is_tagged() && tag() == 5; }
    [[nodiscard]] HeapObject* as_heap_ptr()  const noexcept;

    // Numeric value as double for arithmetic (int32 promotes to double).
    [[nodiscard]] double as_number() const noexcept;

    bool operator==(const Value&) const noexcept = default;
};

// Startup guard: aborts (with a diagnostic) if a GC allocation address does not
// fit in the 48-bit HeapPtr payload (Req from design §1.3).
void assert_heap_ptr_fits(const void* p);

} // namespace malibu::js::vm
