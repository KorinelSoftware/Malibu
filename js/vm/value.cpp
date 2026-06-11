// js/vm/value.cpp
// NaN-boxed Value implementation.

#include "malibu/js/vm/value.h"
#include "malibu/diagnostics/diagnostic_log.h"

#include <bit>
#include <cmath>
#include <cstdlib>

namespace malibu::js::vm {

namespace {
constexpr uint64_t tagged(uint8_t tag, uint64_t payload) noexcept {
    return Value::kQuietNan | (static_cast<uint64_t>(tag & 0x7) << 48)
           | (payload & Value::kPayloadMask);
}
}  // namespace

Value Value::make_double(double d) noexcept {
    Value v;
    // Canonicalise NaNs so they never alias a tagged value.
    v.raw = std::isnan(d) ? kQuietNan : std::bit_cast<uint64_t>(d);
    return v;
}

Value Value::make_int32(int32_t i) noexcept {
    Value v; v.raw = tagged(1, static_cast<uint32_t>(i)); return v;
}
Value Value::make_bool(bool b) noexcept {
    Value v; v.raw = tagged(2, b ? 1u : 0u); return v;
}
Value Value::make_null() noexcept {
    Value v; v.raw = tagged(3, 0); return v;
}
Value Value::make_undefined() noexcept {
    Value v; v.raw = tagged(4, 0); return v;
}
Value Value::make_heap_ptr(HeapObject* p) noexcept {
    Value v;
    v.raw = tagged(5, reinterpret_cast<uintptr_t>(p) & kPayloadMask);
    return v;
}

bool Value::is_tagged() const noexcept {
    // Tagged iff a quiet NaN whose top 16 bits select tags 1..7 (0x7FF9..0x7FFF).
    uint16_t top = static_cast<uint16_t>(raw >> 48);
    return top >= 0x7FF9 && top <= 0x7FFF;
}

uint8_t Value::tag() const noexcept {
    return static_cast<uint8_t>((raw >> 48) & 0x7);
}

double Value::as_double() const noexcept {
    return std::bit_cast<double>(raw);
}
int32_t Value::as_int32() const noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(raw & 0xFFFFFFFFu));
}
bool Value::as_bool() const noexcept {
    return (raw & 0x1) != 0;
}
HeapObject* Value::as_heap_ptr() const noexcept {
    if (!is_heap_ptr()) return nullptr;
    return reinterpret_cast<HeapObject*>(static_cast<uintptr_t>(raw & kPayloadMask));
}

double Value::as_number() const noexcept {
    if (is_int32()) return static_cast<double>(as_int32());
    if (is_double()) return as_double();
    if (is_bool()) return as_bool() ? 1.0 : 0.0;
    if (is_null()) return 0.0;
    return std::nan("");  // undefined / objects → NaN
}

void assert_heap_ptr_fits(const void* p) {
    if ((reinterpret_cast<uintptr_t>(p) & ~Value::kPayloadMask) != 0) {
        MALIBU_LOG(ERROR, "vm",
                   "GC allocation above 2^48 cannot be NaN-boxed; aborting");
        std::abort();
    }
}

} // namespace malibu::js::vm
