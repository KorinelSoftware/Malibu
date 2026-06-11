// core/event_loop/event_loop.cpp
// Deterministic HTML event loop with a virtual clock.

#include "malibu/event_loop/event_loop.h"

#include <algorithm>

namespace malibu::event_loop {

void EventLoop::post_task(Task task) { tasks_.push_back(std::move(task)); }

uint64_t EventLoop::set_timeout(Task task, uint64_t delay_ms) {
    uint64_t id = next_id_++;
    timers_.push_back(Timer{id, now_ms_ + delay_ms, 0, std::move(task), true});
    return id;
}

uint64_t EventLoop::set_interval(Task task, uint64_t interval_ms) {
    uint64_t id = next_id_++;
    // Intervals fire no sooner than 1ms apart to guarantee progress.
    uint64_t iv = interval_ms == 0 ? 1 : interval_ms;
    timers_.push_back(Timer{id, now_ms_ + iv, iv, std::move(task), true});
    return id;
}

void EventLoop::clear_timer(uint64_t id) {
    for (auto& t : timers_) if (t.id == id) t.active = false;
}

void EventLoop::queue_microtask(Task task) { microtasks_.push_back(std::move(task)); }

uint64_t EventLoop::request_animation_frame(FrameTask cb) {
    uint64_t id = next_id_++;
    anim_callbacks_.push_back(AnimCallback{id, std::move(cb)});
    return id;
}

void EventLoop::cancel_animation_frame(uint64_t id) {
    anim_callbacks_.erase(std::remove_if(anim_callbacks_.begin(), anim_callbacks_.end(),
                                         [id](const AnimCallback& a) { return a.id == id; }),
                          anim_callbacks_.end());
}

void EventLoop::drain_microtasks() {
    // Drain internal microtasks and the external (JS) microtask source until
    // both are empty — a microtask may enqueue further microtasks.
    for (;;) {
        bool did_work = false;
        while (!microtasks_.empty()) {
            Task t = std::move(microtasks_.front());
            microtasks_.pop_front();
            if (t) t();
            did_work = true;
        }
        if (microtask_drainer_) { microtask_drainer_(); }
        if (!did_work || microtasks_.empty()) {
            // If the external drainer queued new microtasks, loop again.
            if (microtasks_.empty()) break;
        }
    }
}

int EventLoop::next_due_timer() const {
    int best = -1;
    uint64_t best_at = 0;
    for (size_t i = 0; i < timers_.size(); ++i) {
        if (!timers_[i].active) continue;
        if (best == -1 || timers_[i].fire_at < best_at) { best = static_cast<int>(i); best_at = timers_[i].fire_at; }
    }
    return best;
}

void EventLoop::render_step() {
    // Run the current batch of animation-frame callbacks, then paint.
    std::vector<AnimCallback> batch;
    batch.swap(anim_callbacks_);
    for (auto& a : batch) if (a.cb) a.cb(static_cast<double>(now_ms_));
    drain_microtasks();
    if (paint_callback_) paint_callback_();
}

bool EventLoop::run_one_iteration() {
    if (quitting_) return false;

    if (!tasks_.empty()) {
        Task t = std::move(tasks_.front());
        tasks_.pop_front();
        if (t) t();
        drain_microtasks();
        return true;
    }

    int ti = next_due_timer();
    if (ti >= 0) {
        Timer timer = timers_[static_cast<size_t>(ti)];  // copy
        if (timer.fire_at > now_ms_) now_ms_ = timer.fire_at;  // advance virtual clock
        if (timer.interval > 0) {
            timers_[static_cast<size_t>(ti)].fire_at = now_ms_ + timer.interval;  // reschedule
        } else {
            timers_[static_cast<size_t>(ti)].active = false;
        }
        if (timer.task) timer.task();
        drain_microtasks();
        // Clean up inactive one-shot timers periodically.
        timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
                                     [](const Timer& t) { return !t.active; }),
                      timers_.end());
        return true;
    }

    if (!anim_callbacks_.empty()) {
        now_ms_ += 16;  // ~60fps rendering opportunity
        render_step();
        return true;
    }
    return false;
}

void EventLoop::run_until_idle(uint64_t max_iterations) {
    drain_microtasks();
    uint64_t n = 0;
    while (!quitting_ && run_one_iteration()) {
        if (++n >= max_iterations) break;
    }
}

void EventLoop::run_ready_tasks(uint64_t max_iterations) {
    drain_microtasks();
    bool rendered = false;
    uint64_t iterations = 0;
    while (!quitting_ && iterations++ < max_iterations) {
        if (!tasks_.empty()) {
            Task task = std::move(tasks_.front());
            tasks_.pop_front();
            if (task) task();
            drain_microtasks();
            continue;
        }

        int timer_index = next_due_timer();
        if (timer_index >= 0 &&
            timers_[static_cast<size_t>(timer_index)].fire_at <= now_ms_) {
            Timer timer = timers_[static_cast<size_t>(timer_index)];
            if (timer.interval > 0) {
                timers_[static_cast<size_t>(timer_index)].fire_at =
                    now_ms_ + timer.interval;
            } else {
                timers_[static_cast<size_t>(timer_index)].active = false;
            }
            if (timer.task) timer.task();
            drain_microtasks();
            timers_.erase(
                std::remove_if(timers_.begin(), timers_.end(),
                               [](const Timer& item) { return !item.active; }),
                timers_.end());
            continue;
        }

        if (!rendered && !anim_callbacks_.empty()) {
            rendered = true;
            render_step();
            continue;
        }
        break;
    }
}

bool EventLoop::has_pending() const {
    if (!tasks_.empty() || !microtasks_.empty() || !anim_callbacks_.empty()) return true;
    for (const auto& t : timers_) if (t.active) return true;
    return false;
}

} // namespace malibu::event_loop
