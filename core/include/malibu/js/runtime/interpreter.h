#pragma once
// core/include/malibu/js/runtime/interpreter.h
// Tree of compiled Functions executed by a register-based interpreter with
// lexical environments, prototype-based property access, exceptions, and a
// mark-sweep heap. This is the MalibuJS runtime (Tasks 16/17, full).

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "malibu/js/vm/value.h"
#include "malibu/js/heap/heap.h"
#include "malibu/js/runtime/objects.h"
#include "malibu/js/compiler/compiler.h"

namespace malibu::event_loop { class EventLoop; }

namespace malibu::js::runtime {

using vm::Value;

// Thrown to unwind the C++ stack when a JS exception is not handled in the
// current frame; carries the thrown JS value.
struct ThrowSignal { Value value; };

// Thrown by an async frame at an `await` to unwind the C++ stack while the
// frame's state is preserved on the heap for later resumption.
struct SuspendSignal {};

// Thrown by a generator frame at a `yield` to unwind back to the on-demand
// driver (gen.next()). The yielded value is left on the frame.
struct YieldSignal {};

// Optional sink for console output (captured by tests).
struct ConsoleSink {
    virtual ~ConsoleSink() = default;
    virtual void log(const std::string& line) = 0;
};

class Interpreter {
public:
    explicit Interpreter(heap::Heap& heap);

    Environment*  global() noexcept { return global_; }
    heap::Heap&   heap() noexcept { return heap_; }

    // Runs a top-level compiled function (the program). Returns its completion
    // value. Throws ThrowSignal on an uncaught JS exception.
    Value run_program(const compiler::Function* program);

    // Calls a callable JS value. Throws ThrowSignal on an uncaught exception.
    Value call(Value callee, Value this_val, std::vector<Value>& args);

    // Constructs via `new`. Throws ThrowSignal on error.
    Value construct(Value callee, std::vector<Value>& args);

    // ---- allocation helpers (used by builtins) ----
    JSString*   new_string(std::u16string s);
    JSObject*   new_object();
    JSArray*    new_array();
    JSMap*      new_map();
    JSSet*      new_set();
    JSFunction* new_native(const std::u16string& name, NativeFn fn, uint32_t arity = 0);
    Value       str(const std::u16string& s) { return Value::make_heap_ptr(new_string(s)); }
    Value       str(const std::string& s);

    // ---- conversions / semantics (used by builtins) ----
    std::u16string to_string(Value v);
    double         to_number(Value v);
    bool           to_bool(Value v);
    int32_t        to_int32(Value v);
    std::u16string js_typeof(Value v);
    bool           strict_equals(Value a, Value b);
    bool           loose_equals(Value a, Value b);
    std::vector<Value> to_values(Value v);  // flatten any iterable to a value list
    Value deep_clone(Value v);              // structuredClone semantics

    // eval / Function: the Engine installs a hook that parses+compiles+runs a
    // source string in the global realm (used by the `eval` and `Function`
    // builtins, which live in the runtime but need the parser/compiler).
    void  set_eval_hook(std::function<Value(const std::u16string&)> h) { eval_hook_ = std::move(h); }
    Value run_eval(const std::u16string& src) {
        if (eval_hook_) return eval_hook_(src);
        throw_error(u"EvalError", u"eval is not available");
        return Value::make_undefined();
    }
    // Drives a generator one step (next/return/throw). Returns an IteratorResult.
    Value gen_resume(JSGenerator* gen, Value value, bool is_throw, bool is_return);

    // Raises a JS exception (Error-like object) — never returns.
    [[noreturn]] void throw_error(const std::u16string& kind, const std::u16string& message);

    void set_console_sink(ConsoleSink* sink) { console_ = sink; }
    ConsoleSink* console_sink() const { return console_; }

    // DOM integration (optional). The binding layer installs hooks that route
    // property access on DOM node values through the WebCall ABI. With no hooks
    // installed the engine runs pure JS with no DOM dependency.
    using DomGetFn = std::function<bool(Interpreter&, Value node, const std::u16string& name, Value& out)>;
    using DomSetFn = std::function<bool(Interpreter&, Value node, const std::u16string& name, Value v)>;
    void set_dom_hooks(DomGetFn get, DomSetFn set) {
        dom_get_hook_ = std::move(get);
        dom_set_hook_ = std::move(set);
    }

    JSObject* object_proto() { return object_proto_; }
    JSObject* array_proto()  { return array_proto_; }
    JSObject* string_proto() { return string_proto_; }

    // Public helpers used by builtins.
    void           set_prop_public(Value obj, const std::u16string& name, Value v) { set_prop(obj, name, v); }
    Value          get_prop_public(Value obj, const std::u16string& name) { return get_prop(obj, name); }
    std::u16string json_stringify(Value v);

    // GC rooting for natives that hold heap values across nested calls.
    void push_root(Value v) { temp_roots_.push_back(v); }
    void pop_root() { if (!temp_roots_.empty()) temp_roots_.pop_back(); }

    // Long-lived host roots (e.g. pending timer callbacks) kept alive until run.
    void add_host_root(Value v) { host_roots_.push_back(v); }
    void remove_host_root(Value v);

    // Returns the (cached, stable) DOM node wrapper for a handle, so that
    // `a === b` holds for the same node and expando properties persist.
    Value make_dom_node(malibu::NodeHandle h);
    // Drops the wrapper cache (call on navigation / document reset).
    void  clear_dom_wrappers() { dom_wrappers_.clear(); }

    // ---- Promises / microtasks (Task 17 async) ----
    using MicrotaskFn = std::function<void()>;
    void enqueue_microtask(MicrotaskFn fn, std::vector<Value> roots = {});
    void run_microtasks();
    [[nodiscard]] bool has_pending_microtasks() const noexcept { return !microtasks_.empty(); }

    JSPromise* new_promise();
    void       resolve_promise(JSPromise* p, Value value);
    void       reject_promise(JSPromise* p, Value reason);
    Value      promise_then(JSPromise* p, Value on_fulfilled, Value on_rejected);
    JSPromise* promise_resolve_value(Value v);     // Promise.resolve(v)
    // If `v` is a promise, drains microtasks and returns its fulfilled value
    // (throws ThrowSignal on rejection, undefined if still pending). Otherwise
    // returns `v` unchanged. Used by hosts that need a settled result.
    Value      await_settled(Value v);
    [[nodiscard]] bool is_callable(Value v) const;
    [[nodiscard]] bool is_promise(Value v) const;
    JSObject*  promise_proto() { return promise_proto_; }

    // ---- event loop integration (for setTimeout / setInterval / rAF) ----
    void set_event_loop(malibu::event_loop::EventLoop* loop) { loop_ = loop; }
    [[nodiscard]] malibu::event_loop::EventLoop* event_loop() const noexcept { return loop_; }

private:
    struct Handler {
        uint8_t      flags;       // b0 = has catch, b1 = has finally
        int          catch_pc;
        int          finally_pc;
        uint8_t      exc_reg;
        Environment* saved_env;
    };

    struct Frame {
        const compiler::Function* fn = nullptr;
        std::vector<Value>        regs;
        Environment*              env = nullptr;
        Value                     this_val;
        size_t                    pc = 0;
        std::vector<Handler>      handlers;
        bool                      pending_exc = false;
        Value                     exc_value;
        bool                      returning = false;
        Value                     return_value;

        // async/await state (heap-allocated frames only)
        bool                      is_async = false;
        JSPromise*                async_result = nullptr;
        JSPromise*                await_promise = nullptr;  // promise awaited at suspension
        bool                      has_resume_value = false;
        Value                     resume_value;
        bool                      resume_is_throw = false;

        // generator state (heap-allocated frames only)
        bool                      is_generator = false;
        bool                      started = false;     // first next() ran?
        Value                     yield_value;
    };

    struct Microtask {
        MicrotaskFn        run;
        std::vector<Value> roots;  // kept alive while this task is pending
    };

    Value run_frame(Frame& frame);
    bool  unwind_to_handler(Frame& frame, Value exc);

    // async machinery
    Value call_async(JSFunction* fn, Value this_val, std::vector<Value>& args);
    void  drive_async(std::shared_ptr<Frame> sf);
    void  resume_async(std::shared_ptr<Frame> sf, Value value, bool is_throw);

    // generators (reuse the suspend/resume frame machinery, driven on demand)
    Value make_generator(JSFunction* fn, Value this_val, std::vector<Value>& args);
    JSObject* generator_proto_ = nullptr;
    std::vector<std::weak_ptr<Frame>> gen_frames_;  // GC-rooted while alive
    void  settle_promise(JSPromise* p, JSPromise::State state, Value value);
    void  install_promise_proto();

    // property / element access
    Value get_prop(Value obj, const std::u16string& name);
    void  set_prop(Value obj, const std::u16string& name, Value v, bool enumerable = true);
    Value object_get(JSObject* obj, const std::u16string& name, Value receiver);  // accessor-aware
    void  object_set(JSObject* obj, const std::u16string& name, Value v, Value receiver, bool enumerable = true);
    Value get_elem(Value obj, Value key);
    void  set_elem(Value obj, Value key, Value v, bool enumerable = true);
    Value array_or_string_length(Value v);
    Value make_iterable(Value v, bool keys);
    void  drive_iterator(Value iter, JSArray* out);  // runs the iterator protocol
    // Flattens an array Value into a positional argument list (spread / super).
    std::vector<Value> spread_args(Value array);

    // DOM helpers (implemented in dom_bindings.cpp; no-ops if dom_ctx_ null)
    bool  is_dom_node(Value v);
    Value dom_get_prop(Value node, const std::u16string& name);
    bool  dom_set_prop(Value node, const std::u16string& name, Value v);

    void  install_builtins();
    void  install_array_proto();
    void  install_string_proto();
    void  install_typed_arrays();   // ArrayBuffer / TypedArrays / DataView (typed_arrays.cpp)

    // Binary-data element access (defined in typed_arrays.cpp).
    Value ta_get_index(JSTypedArray* ta, size_t idx);
    void  ta_set_index(JSTypedArray* ta, size_t idx, Value v);
    void  mark_roots(const heap::Heap::MarkFn& mark);

    std::u16string number_to_string(double d);

    heap::Heap&          heap_;
    Environment*         global_ = nullptr;
    JSObject*            global_object_ = nullptr;  // globalThis / top-level `this`
    JSObject*            object_proto_ = nullptr;
    JSObject*            array_proto_ = nullptr;
    JSObject*            string_proto_ = nullptr;
    JSObject*            promise_proto_ = nullptr;
    JSObject*            map_proto_ = nullptr;
    JSObject*            set_proto_ = nullptr;
    JSObject*            number_proto_ = nullptr;
    JSObject*            boolean_proto_ = nullptr;
    JSObject*            symbol_proto_ = nullptr;
    JSObject*            array_buffer_proto_ = nullptr;
    JSObject*            typed_array_proto_ = nullptr;   // %TypedArray%.prototype
    JSObject*            data_view_proto_ = nullptr;
    std::function<Value(const std::u16string&)> eval_hook_;
    JSObject*            function_proto_ = nullptr;  // call/apply/bind for all functions
    std::vector<Frame*>  frame_stack_;
    std::vector<std::shared_ptr<Frame>> suspended_frames_;  // rooted while awaiting
    std::deque<Microtask> microtasks_;
    std::vector<Value>   temp_roots_;   // native scratch values kept alive across allocs
    std::unordered_map<uint64_t, vm::DomNodeRef*> dom_wrappers_;  // stable node-wrapper cache
    std::vector<Value>   host_roots_;   // pending timer / rAF callbacks
    ConsoleSink*         console_ = nullptr;
    DomGetFn             dom_get_hook_;
    DomSetFn             dom_set_hook_;
    malibu::event_loop::EventLoop* loop_ = nullptr;
    int                  call_depth_ = 0;

    friend struct Builtins;
};

} // namespace malibu::js::runtime
