// tests/test_discord_phase4.cpp
// Phase 4: generators + the iteration protocol. Redux-saga-style code, lazy
// sequences, and custom iterables (anything with [Symbol.iterator]/next) must
// work with for-of, spread, and destructuring.

#include <gtest/gtest.h>
#include "malibu/view/view.h"

using malibu::view::View;

namespace {
constexpr const char* kShell = "<!doctype html><html><body></body></html>";
::testing::AssertionResult Eval(View& v, const char* label, const std::string& src,
                                const std::string& expected) {
    std::string r = v.eval_js(src);
    if (r == expected) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << "phase4 [" << label << "]: got " << r << ", expected " << expected;
}
}  // namespace

TEST(DiscordPhase4, Generators) {
    View v;
    ASSERT_TRUE(v.load_html(kShell, "https://discord.com/app"));

    EXPECT_TRUE(Eval(v, "basic-yield", R"JS(
        function* g() { yield 1; yield 2; yield 3; }
        var it = g();
        '' + it.next().value + it.next().value + it.next().value + it.next().done;
    )JS", "\"123true\""));

    EXPECT_TRUE(Eval(v, "for-of-generator", R"JS(
        function* range(n) { for (var i = 0; i < n; i++) yield i * 2; }
        var sum = 0;
        for (var x of range(5)) sum += x;
        sum;
    )JS", "20"));

    EXPECT_TRUE(Eval(v, "spread-generator", R"JS(
        function* chars() { yield 'a'; yield 'b'; yield 'c'; }
        [...chars()].join('-');
    )JS", "\"a-b-c\""));

    EXPECT_TRUE(Eval(v, "yield-receives-value", R"JS(
        function* echo() { var x = yield 'first'; var y = yield x; return y; }
        var it = echo();
        var r0 = it.next();        // {first, false}
        var r1 = it.next(10);      // x=10 -> yield 10
        var r2 = it.next(20);      // y=20 -> return 20
        '' + r0.value + ':' + r1.value + ':' + r2.value + ':' + r2.done;
    )JS", "\"first:10:20:true\""));

    EXPECT_TRUE(Eval(v, "yield-star", R"JS(
        function* inner() { yield 1; yield 2; }
        function* outer() { yield 0; yield* inner(); yield 3; }
        [...outer()].join(',');
    )JS", "\"0,1,2,3\""));
}

TEST(DiscordPhase4, IteratorProtocol) {
    View v;
    ASSERT_TRUE(v.load_html(kShell, "https://discord.com/app"));

    // A custom iterable via [Symbol.iterator] returning a hand-written iterator.
    EXPECT_TRUE(Eval(v, "custom-iterable", R"JS(
        var counter = {
            from: 1, to: 3,
            [Symbol.iterator]() {
                var cur = this.from, last = this.to;
                return { next() { return cur <= last ? { value: cur++, done: false } : { value: undefined, done: true }; } };
            }
        };
        [...counter].join(',');
    )JS", "\"1,2,3\""));

    // Destructuring consumes the iterator protocol.
    EXPECT_TRUE(Eval(v, "destructure-iterable", R"JS(
        function* g() { yield 10; yield 20; yield 30; }
        var [a, b] = g();
        a + b;
    )JS", "30"));

    // Map iteration yields [k, v] pairs.
    EXPECT_TRUE(Eval(v, "for-of-map", R"JS(
        var m = new Map([['a', 1], ['b', 2]]);
        var out = '';
        for (var [k, val] of m) out += k + val;
        out;
    )JS", "\"a1b2\""));
}
