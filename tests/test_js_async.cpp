// tests/test_js_async.cpp
// Task 17 (async): Promises, microtask ordering, async/await, and timers wired
// to the event loop.

#include <gtest/gtest.h>
#include "malibu/js/engine.h"

#include <string>
#include <vector>

using malibu::js::Engine;

namespace {
struct CaptureSink : malibu::js::runtime::ConsoleSink {
    std::vector<std::string> lines;
    void log(const std::string& line) override { lines.push_back(line); }
};
}  // namespace

TEST(JSAsync, PromiseThenResolves) {
    Engine e;
    auto r = e.evaluate(
        "let out = 0;"
        "Promise.resolve(41).then(v => { out = v + 1; });"
        "out");
    ASSERT_TRUE(r.ok) << r.error;
    // Reaction runs as a microtask (drained after evaluate).
    auto r2 = e.evaluate("out");
    EXPECT_EQ(e.interpreter().to_string(r2.value), u"42");
}

TEST(JSAsync, PromiseChaining) {
    Engine e;
    e.evaluate(
        "globalThis2 = 0;"
        "let result = 0;"
        "Promise.resolve(1)"
        "  .then(v => v + 1)"
        "  .then(v => v * 10)"
        "  .then(v => { result = v; });");
    auto r = e.evaluate("result");
    EXPECT_EQ(e.interpreter().to_string(r.value), u"20");
}

TEST(JSAsync, PromiseCatch) {
    Engine e;
    e.evaluate(
        "let msg = '';"
        "Promise.reject('boom').catch(e => { msg = 'caught ' + e; });");
    auto r = e.evaluate("msg");
    EXPECT_EQ(e.interpreter().to_string(r.value), u"caught boom");
}

TEST(JSAsync, MicrotaskOrdering) {
    Engine e;
    CaptureSink sink;
    e.set_console_sink(&sink);
    e.evaluate(
        "console.log('start');"
        "Promise.resolve().then(() => console.log('micro1')).then(() => console.log('micro2'));"
        "queueMicrotask(() => console.log('queued'));"
        "console.log('end');");
    // Order: synchronous logs first, then microtasks in FIFO/chained order.
    ASSERT_GE(sink.lines.size(), 5u);
    EXPECT_EQ(sink.lines[0], "start");
    EXPECT_EQ(sink.lines[1], "end");
    EXPECT_EQ(sink.lines[2], "micro1");
    EXPECT_EQ(sink.lines[3], "queued");
    EXPECT_EQ(sink.lines[4], "micro2");
}

TEST(JSAsync, AsyncAwaitReturnsValue) {
    Engine e;
    e.evaluate(
        "let result = 0;"
        "async function compute() { let a = await 20; let b = await 22; return a + b; }"
        "compute().then(v => { result = v; });");
    auto r = e.evaluate("result");
    EXPECT_EQ(e.interpreter().to_string(r.value), u"42");
}

TEST(JSAsync, AsyncAwaitChainsPromises) {
    Engine e;
    e.evaluate(
        "let out = 0;"
        "function delay(v) { return Promise.resolve(v); }"
        "async function run() {"
        "  let x = await delay(5);"
        "  let y = await delay(x * 2);"
        "  return y + 1;"
        "}"
        "run().then(v => { out = v; });");
    auto r = e.evaluate("out");
    EXPECT_EQ(e.interpreter().to_string(r.value), u"11");
}

TEST(JSAsync, AsyncAwaitTryCatch) {
    Engine e;
    e.evaluate(
        "let caught = '';"
        "async function f() {"
        "  try { await Promise.reject('bad'); } catch (err) { caught = 'got ' + err; }"
        "}"
        "f();");
    auto r = e.evaluate("caught");
    EXPECT_EQ(e.interpreter().to_string(r.value), u"got bad");
}

TEST(JSAsync, SetTimeoutOrderingWithMicrotasks) {
    Engine e;
    CaptureSink sink;
    e.set_console_sink(&sink);
    e.evaluate(
        "console.log('script start');"
        "setTimeout(() => console.log('timeout'), 0);"
        "Promise.resolve().then(() => console.log('promise'));"
        "console.log('script end');");
    e.run_event_loop();  // fire timers + drain microtasks
    ASSERT_EQ(sink.lines.size(), 4u);
    EXPECT_EQ(sink.lines[0], "script start");
    EXPECT_EQ(sink.lines[1], "script end");
    EXPECT_EQ(sink.lines[2], "promise");   // microtask before the timer task
    EXPECT_EQ(sink.lines[3], "timeout");
}

TEST(JSAsync, PromiseAll) {
    Engine e;
    e.evaluate(
        "let total = 0;"
        "Promise.all([Promise.resolve(1), Promise.resolve(2), Promise.resolve(3)])"
        "  .then(arr => { total = arr[0] + arr[1] + arr[2]; });");
    e.run_event_loop();
    auto r = e.evaluate("total");
    EXPECT_EQ(e.interpreter().to_string(r.value), u"6");
}

TEST(JSAsync, SetTimeoutWithDelayFires) {
    Engine e;
    e.evaluate(
        "let fired = false;"
        "setTimeout(() => { fired = true; }, 50);");
    e.run_event_loop();
    auto r = e.evaluate("fired");
    EXPECT_EQ(e.interpreter().to_string(r.value), u"true");
}
