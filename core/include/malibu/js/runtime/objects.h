#pragma once
// core/include/malibu/js/runtime/objects.h
// Heap object model for the MalibuJS runtime: strings, objects, arrays,
// functions (script + native), and lexical environments. All derive from
// vm::HeapObject so the GC can trace and reclaim them uniformly.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gmpxx.h>

#include "malibu/js/vm/value.h"

namespace malibu::js::compiler { struct Function; }

namespace malibu::js::runtime {

using vm::Value;
using vm::HeapObject;

class Interpreter;  // defined in vm/interpreter.h

// ---------------------------------------------------------------------------
// String
// ---------------------------------------------------------------------------
struct JSString : HeapObject {
    std::u16string data;
    JSString() { kind = kJSString; }
    explicit JSString(std::u16string s) : data(std::move(s)) { kind = kJSString; }
};

// BigInt is a primitive in ECMAScript. It is represented by an immutable
// heap payload so it can retain arbitrary precision while still fitting in
// the NaN-boxed Value representation.
struct JSBigInt : HeapObject {
    mpz_class value;
    JSBigInt() { kind = kJSBigInt; }
    explicit JSBigInt(mpz_class v) : value(std::move(v)) { kind = kJSBigInt; }
};

// Symbols are primitives with identity. `property_key` is the engine-internal
// key used by computed property access; it is never exposed as the symbol's
// description.
struct JSSymbol : HeapObject {
    std::u16string description;
    std::u16string property_key;
    std::u16string registry_key;
    bool           registered = false;

    JSSymbol() { kind = kJSSymbol; }
    JSSymbol(std::u16string desc, std::u16string key)
        : description(std::move(desc)), property_key(std::move(key)) {
        kind = kJSSymbol;
    }
};

// ---------------------------------------------------------------------------
// Object — insertion-ordered string-keyed property map with a prototype link.
// ---------------------------------------------------------------------------
struct Property {
    std::u16string key;
    Value          value;
    bool           enumerable = true;
    // Accessor (get/set) support. When is_accessor, `value` is unused and the
    // interpreter invokes getter/setter (callables) on access.
    bool           is_accessor = false;
    Value          getter;
    Value          setter;
    bool           writable = true;
    bool           configurable = true;
};

struct JSObject : HeapObject {
    std::vector<Property> props;
    JSObject*             proto = nullptr;
    // Integrity level (Object.preventExtensions / seal / freeze). `extensible`
    // gates adding new own properties; `frozen` additionally blocks mutation of
    // existing ones.
    bool                  extensible = true;
    bool                  sealed = false;
    bool                  frozen = false;

    JSObject() { kind = kJSObject; }

    Property*       find_own(std::u16string_view key);
    const Property* find_own(std::u16string_view key) const;
    // Own-or-inherited property lookup (walks the prototype chain).
    Property*       resolve(std::u16string_view key);
    // Installs/updates an accessor (getter or setter) on this object.
    void            define_accessor(std::u16string_view key, Value fn, bool is_setter,
                                    bool enumerable = false);

    // Own-or-inherited get. Returns undefined if absent.
    Value get(std::u16string_view key) const;
    bool  has(std::u16string_view key) const;          // own or inherited
    bool  has_own(std::u16string_view key) const;
    void  set(std::u16string_view key, Value v, bool enumerable = true);
    bool  delete_prop(std::u16string_view key);
    std::vector<std::u16string> own_enumerable_keys() const;
};

// ---------------------------------------------------------------------------
// Array — dense integer-indexed elements plus inherited object properties.
// ---------------------------------------------------------------------------
struct JSArray : JSObject {
    std::vector<Value> elements;
    // Empty means every element is present. Once an array becomes sparse this
    // mirrors `elements`; 0 denotes an absent integer-indexed property.
    std::vector<uint8_t> presence;
    bool               length_writable = true;
    JSArray() { kind = kJSArray; }

    [[nodiscard]] bool has_index(size_t index) const noexcept;
    void append(Value value, bool present = true);
    void resize_length(size_t length, bool new_indices_present = false);
    void set_index(size_t index, Value value);
    void delete_index(size_t index);
    void erase_range(size_t start, size_t count);
    void insert_dense(size_t start, const std::vector<Value>& values);
    void reverse_elements();

private:
    void materialize_presence();
    void normalize_presence();
};

// ---------------------------------------------------------------------------
// Function — either script (compiled bytecode + captured environment) or
// native (C++ callback). Functions may also carry own properties (.prototype).
// ---------------------------------------------------------------------------
struct Environment;

using NativeFn = std::function<Value(Interpreter&, Value /*this*/, std::vector<Value>& /*args*/)>;

struct JSFunction : JSObject {
    std::u16string             name;
    uint32_t                   arity = 0;

    const compiler::Function*  code = nullptr;   // script function
    Environment*               closure = nullptr;

    NativeFn                   native;           // set => native function
    bool                       constructable = true;

    JSFunction() { kind = kJSFunction; }
    [[nodiscard]] bool is_native() const noexcept { return static_cast<bool>(native); }
};

// ---------------------------------------------------------------------------
// Promise — Promises/A+ object with reaction records (Task 17).
// ---------------------------------------------------------------------------
struct JSPromise;

struct PromiseReaction {
    Value       on_fulfilled;   // callable or undefined
    Value       on_rejected;    // callable or undefined
    JSPromise*  result = nullptr;  // promise returned by then()
};

struct JSPromise : JSObject {
    enum class State : uint8_t { Pending, Fulfilled, Rejected };
    State                        state = State::Pending;
    Value                        result;        // value or reason
    std::vector<PromiseReaction> reactions;
    bool                         handled = false;
    JSPromise() { kind = kJSPromise; }
};

// ---------------------------------------------------------------------------
// Map / Set — insertion-ordered collections with arbitrary (SameValueZero) keys.
// Entries are traced by the GC. Backed by a JSObject so they still carry a
// prototype (Map.prototype / Set.prototype) and own properties.
// ---------------------------------------------------------------------------
struct JSMap : JSObject {
    std::vector<std::pair<Value, Value>> entries;
    JSMap() { kind = kJSMap; }
};

struct JSSet : JSObject {
    std::vector<Value> items;
    JSSet() { kind = kJSSet; }
};

// ---------------------------------------------------------------------------
// ArrayBuffer / TypedArray / DataView (ECMAScript binary data).
//   - JSArrayBuffer owns the raw bytes (no JS-value edges).
//   - JSTypedArray / JSDataView are JSObject-derived views (they carry a proto
//     and own properties); integer-indexed access reads/writes the buffer bytes.
// ---------------------------------------------------------------------------
struct JSArrayBuffer : JSObject {
    std::vector<uint8_t> data;
    bool                 detached = false;
    JSArrayBuffer() { kind = kArrayBuffer; }
};

enum class TAKind : uint8_t {
    Int8, Uint8, Uint8Clamped, Int16, Uint16, Int32, Uint32, Float32, Float64,
};

struct JSTypedArray : JSObject {
    JSArrayBuffer* buffer = nullptr;
    size_t         byte_offset = 0;
    size_t         length = 0;     // element count
    TAKind         ta_kind = TAKind::Uint8;
    JSTypedArray() { kind = kTypedArray; }

    static size_t elem_size(TAKind k) {
        switch (k) {
            case TAKind::Int8: case TAKind::Uint8: case TAKind::Uint8Clamped: return 1;
            case TAKind::Int16: case TAKind::Uint16: return 2;
            case TAKind::Int32: case TAKind::Uint32: case TAKind::Float32: return 4;
            case TAKind::Float64: return 8;
        }
        return 1;
    }
    [[nodiscard]] size_t byte_length() const { return length * elem_size(ta_kind); }
};

struct JSDataView : JSObject {
    JSArrayBuffer* buffer = nullptr;
    size_t         byte_offset = 0;
    size_t         byte_length = 0;
    JSDataView() { kind = kDataView; }
};

// Proxy — wraps a target with a handler whose trap functions intercept the
// fundamental object operations (get/set/has/deleteProperty/ownKeys/apply/...).
struct JSProxy : JSObject {
    Value target;
    Value handler;
    JSProxy() { kind = kJSProxy; }
};

// ---------------------------------------------------------------------------
// Generator — an on-demand iterator backed by a suspended interpreter frame.
// The frame is type-erased (Interpreter::Frame is a private nested type); the
// interpreter casts it back. The frame's Values are GC-traced via the
// interpreter's live-generator list, not through this object.
// ---------------------------------------------------------------------------
struct JSGenerator : JSObject {
    std::shared_ptr<void> frame;   // Interpreter::Frame
    bool done    = false;
    bool running = false;          // guards re-entrant next()
    JSGenerator() { kind = kJSGenerator; }
};

// ---------------------------------------------------------------------------
// Environment — a lexical scope record. Closures capture the chain.
// ---------------------------------------------------------------------------
struct Environment : HeapObject {
    Environment*                              parent = nullptr;
    std::vector<std::pair<std::u16string, Value>> slots;
    bool                                      is_function_scope = false;
    // When non-null, this scope's variable bindings live as own properties of
    // `object_backing` (the global object) rather than in `slots`, unifying
    // global `var`/function declarations with `globalThis`/`this`.
    JSObject*                                 object_backing = nullptr;
    // A `with (obj) {}` scope: lookups consult the whole prototype chain of the
    // backing object, not just its own properties.
    bool                                      is_with = false;

    Environment() { kind = kEnvironment; }

    // Nearest enclosing function-scope environment (for `var` hoisting).
    Environment* function_scope() {
        for (Environment* e = this; e; e = e->parent) if (e->is_function_scope) return e;
        return this;
    }

    Value* find(std::u16string_view name);                       // searches the chain
    void   define(std::u16string_view name, Value v);            // in this scope
    void   define_if_absent(std::u16string_view name, Value v);  // in this scope
    bool   set(std::u16string_view name, Value v);               // assigns up the chain
    bool   has(std::u16string_view name) const;
};

} // namespace malibu::js::runtime
