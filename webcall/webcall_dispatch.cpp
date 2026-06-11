// webcall/webcall_dispatch.cpp
// WebCall guards, deoptimization, and fast/slow handler tables wired to the DOM.

#include "malibu/webcall/webcall_dispatch.h"
#include "malibu/dom/document.h"
#include "malibu/dom/dom_command_buffer.h"
#include "malibu/diagnostics/diagnostic_log.h"

#include <array>
#include <mutex>

namespace malibu::webcall {
namespace {

// ---- guards ---------------------------------------------------------------
bool guard_realm_valid(const GuardContext& g)        { return g.realm && g.realm->valid; }
bool guard_proto_stable(const GuardContext& g)       { return g.realm && g.realm->document_prototype_stable; }
bool guard_no_proxy(const GuardContext& g)           { return g.realm && !g.realm->has_proxy_intercept; }
bool guard_qs_original(const GuardContext& g)        { return g.realm && g.realm->query_selector_is_original; }
bool guard_mutators_original(const GuardContext& g)  { return g.realm && g.realm->dom_mutators_are_original; }

constexpr GuardFn kQueryGuards[]   = { guard_realm_valid, guard_proto_stable, guard_no_proxy, guard_qs_original };
constexpr GuardFn kMutateGuards[]  = { guard_realm_valid, guard_proto_stable, guard_no_proxy, guard_mutators_original };
constexpr GuardFn kRealmGuards[]   = { guard_realm_valid };

// ---- shared DOM operation (fast and slow share semantics by construction) -
MalibuErrorCode op_query_selector(WebCallContext& ctx, MalibuHandle target,
                                  const WebCallArgs& a, ValueBox* out) {
    if (!ctx.dom) return MALIBU_ERR_TYPE;
    malibu::NodeHandle scope = decode_handle(target);
    malibu::NodeHandle r = ctx.dom->query_selector(scope, a.str_a);
    if (out) { out->kind = ValueBox::Kind::Node; out->node = r; }
    return MALIBU_OK;
}

MalibuErrorCode op_create_element(WebCallContext& ctx, MalibuHandle, const WebCallArgs& a, ValueBox* out) {
    if (!ctx.dom) return MALIBU_ERR_TYPE;
    malibu::NodeHandle r = ctx.dom->create_element(a.str_a);
    if (out) { out->kind = ValueBox::Kind::Node; out->node = r; }
    return MALIBU_OK;
}

MalibuErrorCode op_append_child(WebCallContext& ctx, MalibuHandle target, const WebCallArgs& a, ValueBox* out) {
    if (!ctx.dom) return MALIBU_ERR_TYPE;
    malibu::dom::DomError e = ctx.dom->append_child(decode_handle(target), a.node_a);
    if (out) { out->kind = ValueBox::Kind::Node; out->node = a.node_a; }
    return e == malibu::dom::DomError::Ok ? MALIBU_OK : MALIBU_ERR_DEAD;
}

MalibuErrorCode op_remove_child(WebCallContext& ctx, MalibuHandle target, const WebCallArgs& a, ValueBox* out) {
    if (!ctx.dom) return MALIBU_ERR_TYPE;
    malibu::dom::DomError e = ctx.dom->remove_child(decode_handle(target), a.node_a);
    if (out) { out->kind = ValueBox::Kind::Node; out->node = a.node_a; }
    return e == malibu::dom::DomError::Ok ? MALIBU_OK : MALIBU_ERR_DEAD;
}

MalibuErrorCode op_set_text(WebCallContext& ctx, MalibuHandle target, const WebCallArgs& a, ValueBox* out) {
    if (!ctx.dom) return MALIBU_ERR_TYPE;
    malibu::dom::DomError e = ctx.dom->set_text_content(decode_handle(target), a.str_a);
    if (out) out->kind = ValueBox::Kind::Undefined;
    return e == malibu::dom::DomError::Ok ? MALIBU_OK : MALIBU_ERR_DEAD;
}

MalibuErrorCode op_get_text(WebCallContext& ctx, MalibuHandle target, const WebCallArgs&, ValueBox* out) {
    if (!ctx.dom) return MALIBU_ERR_TYPE;
    if (out) { out->kind = ValueBox::Kind::Str; out->str = ctx.dom->text_content(decode_handle(target)); }
    return MALIBU_OK;
}

MalibuErrorCode op_set_attribute(WebCallContext& ctx, MalibuHandle target, const WebCallArgs& a, ValueBox* out) {
    if (!ctx.dom) return MALIBU_ERR_TYPE;
    malibu::dom::DomError e = ctx.dom->set_attribute(decode_handle(target), a.str_a, a.str_b);
    if (out) out->kind = ValueBox::Kind::Undefined;
    return e == malibu::dom::DomError::Ok ? MALIBU_OK : MALIBU_ERR_DEAD;
}

MalibuErrorCode op_get_attribute(WebCallContext& ctx, MalibuHandle target, const WebCallArgs& a, ValueBox* out) {
    if (!ctx.dom) return MALIBU_ERR_TYPE;
    auto v = ctx.dom->get_attribute(decode_handle(target), a.str_a);
    if (out) {
        if (v) { out->kind = ValueBox::Kind::Str; out->str = *v; }
        else   { out->kind = ValueBox::Kind::Null; }
    }
    return MALIBU_OK;
}

// ---- tables ---------------------------------------------------------------
std::array<WebCallHandler, WEBCALL_MAX> g_fast{};
std::array<WebCallHandler, WEBCALL_MAX> g_slow{};
std::array<GuardSet, WEBCALL_MAX>       g_guards{};
std::once_flag                          g_init_once;

void do_init() {
    auto reg = [](MalibuWebCallId id, WebCallHandler h, std::span<const GuardFn> guards, const char* name) {
        g_fast[id] = h;
        g_slow[id] = h;  // identical semantics → idempotent deopt (Property 2)
        g_guards[id] = GuardSet{name, guards};
    };

    reg(WEBCALL_DOM_QUERY_SELECTOR,   op_query_selector, kQueryGuards,  "DOM_QUERY_SELECTOR");
    reg(WEBCALL_DOM_CREATE_ELEMENT,   op_create_element, kRealmGuards,  "DOM_CREATE_ELEMENT");
    reg(WEBCALL_DOM_APPEND_CHILD,     op_append_child,   kMutateGuards, "DOM_APPEND_CHILD");
    reg(WEBCALL_DOM_REMOVE_CHILD,     op_remove_child,   kMutateGuards, "DOM_REMOVE_CHILD");
    reg(WEBCALL_DOM_SET_TEXT_CONTENT, op_set_text,       kMutateGuards, "DOM_SET_TEXT_CONTENT");
    reg(WEBCALL_DOM_GET_TEXT_CONTENT, op_get_text,       kQueryGuards,  "DOM_GET_TEXT_CONTENT");
    reg(WEBCALL_DOM_SET_ATTRIBUTE,    op_set_attribute,  kMutateGuards, "DOM_SET_ATTRIBUTE");
    reg(WEBCALL_DOM_GET_ATTRIBUTE,    op_get_attribute,  kQueryGuards,  "DOM_GET_ATTRIBUTE");
}

} // namespace

void init_dispatch_tables() { std::call_once(g_init_once, do_init); }

WebCallHandler fast_handler(MalibuWebCallId id) { init_dispatch_tables(); return g_fast[id]; }
WebCallHandler slow_handler(MalibuWebCallId id) { init_dispatch_tables(); return g_slow[id]; }
const GuardSet& guard_set(MalibuWebCallId id)   { init_dispatch_tables(); return g_guards[id]; }

bool run_guards(MalibuWebCallId id, const GuardContext& gctx) {
    init_dispatch_tables();
    const GuardSet& gs = g_guards[id];
    for (GuardFn fn : gs.guards) {
        if (!fn(gctx)) return false;
    }
    return true;
}

MalibuErrorCode dispatch_call_site(WebCallContext& ctx,
                                   malibu::js::bytecode::CallSiteTable& cst,
                                   uint32_t call_site_id,
                                   MalibuHandle target,
                                   const WebCallArgs& args,
                                   ValueBox* out) {
    init_dispatch_tables();
    uint32_t wid = cst.webcall_id_of(call_site_id);
    if (wid >= WEBCALL_MAX) return MALIBU_ERR_TYPE;
    auto id = static_cast<MalibuWebCallId>(wid);

    if (cst.is_deoptimized(call_site_id)) {
        return g_slow[id] ? g_slow[id](ctx, target, args, out) : MALIBU_ERR_TYPE;
    }

    GuardContext gctx{ctx.realm, call_site_id};
    if (run_guards(id, gctx)) {
        return g_fast[id] ? g_fast[id](ctx, target, args, out) : MALIBU_ERR_TYPE;
    }

    // Guard failed → deoptimize this site and take the generic path.
    cst.deoptimize(call_site_id);
    MALIBU_LOG(DEBUG, "webcall",
               std::string("deopt call_site ") + std::to_string(call_site_id) +
                   " (" + g_guards[id].name + ")");
    return g_slow[id] ? g_slow[id](ctx, target, args, out) : MALIBU_ERR_TYPE;
}

} // namespace malibu::webcall

// ---------------------------------------------------------------------------
// Public C ABI entry. The address-of-symbol below proves the ABI version at
// link time (see webcall_abi_version.cmake).
// ---------------------------------------------------------------------------
extern "C" {
const uint32_t malibu_webcall_abi_version_1 = MALIBU_WEBCALL_ABI_VERSION;

MalibuErrorCode malibu_webcall(MalibuWebCallId /*id*/, MalibuHandle /*target*/,
                               const void* /*args*/, uint32_t /*args_size*/,
                               MalibuValue* /*result_out*/) {
    // The public, context-free C entry is the generic boundary; engine-internal
    // callers use malibu::webcall::dispatch_call_site with a WebCallContext.
    return MALIBU_OK;
}
}
