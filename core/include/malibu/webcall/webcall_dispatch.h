#pragma once
// core/include/malibu/webcall/webcall_dispatch.h
// Internal WebCall dispatch: guards, deoptimization, fast/slow handler tables
// (Task 19). This is the engine-internal C++ surface; the public C ABI is in
// webcall_abi.h.
//
// Correctness-first invariant (Req 19.1-19.3, Property 2):
//   - Every fast WebCall path has a GuardSet evaluated synchronously first.
//   - A guard failure deoptimizes the call site and runs the slow (generic)
//     path, which produces an identical observable result.

#include <cstdint>
#include <string>
#include <span>

#include "malibu/webcall/webcall_abi.h"
#include "malibu/types.h"
#include "malibu/js/bytecode/bytecode.h"

namespace malibu::dom { class DOMTree; class DomCommandBuffer; }

namespace malibu::webcall {

// MalibuValue is opaque in the C ABI; this is its engine-internal definition.
struct ValueBox {
    enum class Kind : uint8_t { Undefined, Null, Node, Bool, Int, Str };
    Kind            kind = Kind::Undefined;
    malibu::NodeHandle node = malibu::NodeHandle::null_handle();
    bool            b = false;
    int32_t         i = 0;
    std::u16string  str;

    bool operator==(const ValueBox& o) const noexcept {
        return kind == o.kind && node == o.node && b == o.b && i == o.i && str == o.str;
    }
};

// Decoded arguments for a WebCall. Not every field is used by every call.
struct WebCallArgs {
    std::u16string     str_a;   // selector / tag / attr-name / text
    std::u16string     str_b;   // attr-value
    malibu::NodeHandle node_a = malibu::NodeHandle::null_handle();  // child node
};

// Per-realm state that guards inspect to decide whether a fast path is valid.
struct RealmState {
    bool valid                     = true;
    bool document_prototype_stable = true;
    bool has_proxy_intercept       = false;
    bool query_selector_is_original = true;
    bool dom_mutators_are_original  = true;
};

// Engine objects a handler needs.
struct WebCallContext {
    malibu::dom::DOMTree*          dom    = nullptr;
    malibu::dom::DomCommandBuffer* cmd    = nullptr;  // optional (batched mutations)
    RealmState*                    realm  = nullptr;
};

struct GuardContext {
    RealmState* realm;
    uint32_t    call_site_id;
};

using GuardFn = bool (*)(const GuardContext&);

struct GuardSet {
    const char*               name;
    std::span<const GuardFn>   guards;  // all must return true to take the fast path
};

using WebCallHandler = MalibuErrorCode (*)(WebCallContext&, MalibuHandle target,
                                           const WebCallArgs&, ValueBox* out);

// Encode / decode a NodeHandle into the opaque MalibuHandle (index<<32|gen).
[[nodiscard]] inline MalibuHandle encode_handle(malibu::NodeHandle h) noexcept {
    return (static_cast<uint64_t>(h.index) << 32) | h.generation;
}
[[nodiscard]] inline malibu::NodeHandle decode_handle(MalibuHandle m) noexcept {
    return malibu::NodeHandle{static_cast<uint32_t>(m >> 32),
                              static_cast<uint32_t>(m & 0xFFFFFFFF)};
}

// Initialise the dispatch + guard tables. Idempotent; safe to call repeatedly.
void init_dispatch_tables();

// Accessors (mainly for tests / VM).
[[nodiscard]] WebCallHandler fast_handler(MalibuWebCallId id);
[[nodiscard]] WebCallHandler slow_handler(MalibuWebCallId id);
[[nodiscard]] const GuardSet& guard_set(MalibuWebCallId id);

// Returns true iff all guards in the set pass for the given context.
[[nodiscard]] bool run_guards(MalibuWebCallId id, const GuardContext& gctx);

// Full guarded dispatch through a call site:
//   1. deoptimized site            -> slow path
//   2. all guards pass             -> fast path
//   3. any guard fails             -> deoptimize(call_site_id) then slow path
MalibuErrorCode dispatch_call_site(WebCallContext& ctx,
                                   malibu::js::bytecode::CallSiteTable& cst,
                                   uint32_t call_site_id,
                                   MalibuHandle target,
                                   const WebCallArgs& args,
                                   ValueBox* out);

} // namespace malibu::webcall
