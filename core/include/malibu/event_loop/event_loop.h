#pragma once
// core/include/malibu/event_loop/event_loop.h
// HTML event loop (Task 21): a task queue (timers + posted tasks), a microtask
// queue drained after every task, animation-frame callbacks, and a paint step.
// A virtual clock makes ordering fully deterministic and testable.

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace malibu::event_loop {

class EventLoop {
public:
    using Task      = std::function<void()>;
    using FrameTask = std::function<void(double timestamp_ms)>;

    // --- macrotasks ---
    void     post_task(Task task);
    uint64_t set_timeout(Task task, uint64_t delay_ms);
    uint64_t set_interval(Task task, uint64_t interval_ms);
    void     clear_timer(uint64_t id);

    // --- microtasks (drained after every task) ---
    void queue_microtask(Task task);
    // External microtask source (e.g. the JS engine's promise reactions),
    // drained as part of every microtask checkpoint.
    void set_microtask_drainer(std::function<void()> drainer) { microtask_drainer_ = std::move(drainer); }

    // --- animation frames + paint ("update the rendering") ---
    uint64_t request_animation_frame(FrameTask cb);
    void     cancel_animation_frame(uint64_t id);
    void     set_paint_callback(std::function<void()> cb) { paint_callback_ = std::move(cb); }

    // --- running ---
    // Runs one task source (task or due timer, else a render step), then a
    // microtask checkpoint. Returns false when the loop is idle.
    bool run_one_iteration();
    // Runs until no work remains (or quit()). Capped to avoid runaway intervals.
    void run_until_idle(uint64_t max_iterations = 1'000'000);
    void drain_microtasks();
    void render_step();
    void quit() { quitting_ = true; }

    [[nodiscard]] bool     has_pending() const;
    [[nodiscard]] uint64_t now_ms() const noexcept { return now_ms_; }
    // For tests: advance the virtual clock without running tasks.
    void advance_clock(uint64_t ms) { now_ms_ += ms; }

private:
    struct Timer {
        uint64_t id;
        uint64_t fire_at;
        uint64_t interval;  // 0 = one-shot
        Task     task;
        bool     active = true;
    };
    struct AnimCallback { uint64_t id; FrameTask cb; };

    [[nodiscard]] int next_due_timer() const;  // index into timers_, or -1

    std::deque<Task>             tasks_;
    std::vector<Timer>           timers_;
    std::deque<Task>             microtasks_;
    std::vector<AnimCallback>    anim_callbacks_;
    std::function<void()>        microtask_drainer_;
    std::function<void()>        paint_callback_;
    uint64_t                     now_ms_  = 0;
    uint64_t                     next_id_ = 1;
    bool                         quitting_ = false;
};

} // namespace malibu::event_loop
