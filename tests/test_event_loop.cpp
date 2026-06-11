// tests/test_event_loop.cpp
// Task 21: microtask checkpoint ordering, timers, and rAF-before-paint.

#include <gtest/gtest.h>
#include "malibu/event_loop/event_loop.h"

#include <string>
#include <vector>

using malibu::event_loop::EventLoop;

TEST(EventLoop, MicrotasksDrainBeforeNextTask) {
    EventLoop loop;
    std::vector<std::string> log;
    loop.post_task([&] {
        log.push_back("task1");
        loop.queue_microtask([&] { log.push_back("micro1"); });
        loop.queue_microtask([&] { log.push_back("micro2"); });
    });
    loop.post_task([&] { log.push_back("task2"); });
    loop.run_until_idle();
    EXPECT_EQ(log, (std::vector<std::string>{"task1", "micro1", "micro2", "task2"}));
}

TEST(EventLoop, MicrotaskCanEnqueueMicrotask) {
    EventLoop loop;
    std::vector<std::string> log;
    loop.post_task([&] {
        loop.queue_microtask([&] {
            log.push_back("a");
            loop.queue_microtask([&] { log.push_back("b"); });
        });
    });
    loop.run_until_idle();
    EXPECT_EQ(log, (std::vector<std::string>{"a", "b"}));
}

TEST(EventLoop, SetTimeoutZeroAfterCurrentTaskAndMicrotasks) {
    EventLoop loop;
    std::vector<std::string> log;
    loop.post_task([&] {
        log.push_back("script start");
        loop.set_timeout([&] { log.push_back("timeout"); }, 0);
        loop.queue_microtask([&] { log.push_back("promise"); });
        log.push_back("script end");
    });
    loop.run_until_idle();
    EXPECT_EQ(log, (std::vector<std::string>{"script start", "script end", "promise", "timeout"}));
}

TEST(EventLoop, TimersFireInDelayOrder) {
    EventLoop loop;
    std::vector<int> order;
    loop.set_timeout([&] { order.push_back(100); }, 100);
    loop.set_timeout([&] { order.push_back(10); }, 10);
    loop.set_timeout([&] { order.push_back(50); }, 50);
    loop.run_until_idle();
    EXPECT_EQ(order, (std::vector<int>{10, 50, 100}));
    EXPECT_GE(loop.now_ms(), 100u);
}

TEST(EventLoop, ClearTimerCancels) {
    EventLoop loop;
    bool fired = false;
    uint64_t id = loop.set_timeout([&] { fired = true; }, 10);
    loop.clear_timer(id);
    loop.run_until_idle();
    EXPECT_FALSE(fired);
}

TEST(EventLoop, AnimationFrameFiresBeforePaint) {
    EventLoop loop;
    std::vector<std::string> log;
    loop.set_paint_callback([&] { log.push_back("paint"); });
    loop.request_animation_frame([&](double) { log.push_back("raf"); });
    loop.run_until_idle();
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0], "raf");
    EXPECT_EQ(log[1], "paint");
}

TEST(EventLoop, IntervalRepeatsUntilCleared) {
    EventLoop loop;
    int count = 0;
    uint64_t id = 0;
    id = loop.set_interval([&] { if (++count >= 3) loop.clear_timer(id); }, 5);
    loop.run_until_idle();
    EXPECT_EQ(count, 3);
}

TEST(EventLoop, ReadyPumpDoesNotDrainPersistentIntervals) {
    EventLoop loop;
    int count = 0;
    loop.set_interval([&] { ++count; }, 5);

    loop.run_ready_tasks();
    EXPECT_EQ(count, 0);
    loop.advance_clock(5);
    loop.run_ready_tasks();
    EXPECT_EQ(count, 1);
    EXPECT_TRUE(loop.has_pending());
}
