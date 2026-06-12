#pragma once
// core/include/malibu/js/engine.h
// MalibuJS engine facade: source -> parse -> compile -> execute.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "malibu/js/heap/heap.h"
#include "malibu/js/runtime/interpreter.h"
#include "malibu/js/compiler/compiler.h"
#include "malibu/event_loop/event_loop.h"

namespace malibu::js {

class Engine {
public:
    Engine();

    struct EvalResult {
        bool           ok = false;
        std::string    error;            // parse/compile/runtime error message
        runtime::Value value;            // completion value (valid when ok)
    };

    // Parses, compiles, and runs `source`. Returns the completion value.
    EvalResult evaluate(std::string_view source, std::string_view filename = "<eval>");
    EvalResult evaluate_module(std::string_view source,
                               std::string_view filename = "<module>");

    // Convenience: evaluate and return the completion value as a string.
    std::string eval_to_string(std::string_view source);

    runtime::Interpreter&        interpreter() noexcept { return interp_; }
    heap::Heap&                  heap() noexcept { return heap_; }
    event_loop::EventLoop&       event_loop() noexcept { return loop_; }

    // Runs the event loop (timers, tasks, rAF) to completion, performing a
    // microtask checkpoint after every task.
    void run_event_loop() { loop_.run_until_idle(); }
    // Host/browser pump: advance by real elapsed time, then run only work that
    // is ready now. Persistent intervals remain scheduled for future pumps.
    void run_ready_tasks(uint64_t elapsed_ms = 0) {
        loop_.advance_clock(elapsed_ms);
        loop_.run_ready_tasks();
    }

    void set_console_sink(runtime::ConsoleSink* sink) { interp_.set_console_sink(sink); }

private:
    EvalResult evaluate_impl(std::string_view source,
                             std::string_view filename,
                             bool isolated_top_level);

    heap::Heap                                              heap_;
    event_loop::EventLoop                                   loop_;
    runtime::Interpreter                                    interp_;
    std::vector<std::shared_ptr<compiler::Function>>        programs_;  // keep bytecode alive
};

} // namespace malibu::js
