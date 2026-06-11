// tests/test_discord_phase5.cpp
// Phase 5: the broad web-platform + JS-builtin surface a real SPA touches at
// boot — Number/Math/String/Array/Object completeness, Promise combinators,
// URL/URLSearchParams, Date, performance, crypto, encoding, location/navigator.

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
           << "phase5 [" << label << "]: got " << r << ", expected " << expected;
}
}  // namespace

TEST(DiscordPhase5, JsBuiltins) {
    View v;
    ASSERT_TRUE(v.load_html(kShell, "https://discord.com/app"));

    EXPECT_TRUE(Eval(v, "number-statics", R"JS(
        '' + Number.isInteger(5) + Number.isInteger(5.5) + (Number.MAX_SAFE_INTEGER === 9007199254740991) + Number.isNaN(NaN);
    )JS", "\"truefalsetruetrue\""));

    EXPECT_TRUE(Eval(v, "math-extras", R"JS(
        '' + Math.trunc(4.7) + ':' + Math.sign(-3) + ':' + Math.hypot(3, 4) + ':' + Math.log2(8);
    )JS", "\"4:-1:5:3\""));

    EXPECT_TRUE(Eval(v, "string-methods", R"JS(
        '5'.padStart(3, '0') + '|' + 'ab'.padEnd(4, '!') + '|' + '  x  '.trimStart().trimEnd() + '|' + 'hello'.at(-1);
    )JS", "\"005|ab!!|x|o\""));

    EXPECT_TRUE(Eval(v, "array-sort-at", R"JS(
        var xs = [3, 1, 2, 10];
        xs.sort((a, b) => a - b).join(',') + '|' + [5, 6, 7].at(-2);
    )JS", "\"1,2,3,10|6\""));

    EXPECT_TRUE(Eval(v, "object-defineProperty", R"JS(
        var o = {};
        Object.defineProperty(o, 'x', { get() { return 42; } });
        Object.defineProperty(o, 'y', { value: 7, enumerable: true });
        '' + o.x + ':' + o.y + ':' + Object.getOwnPropertyNames(o).join(',');
    )JS", "\"42:7:x,y\""));

    EXPECT_TRUE(Eval(v, "encoding", R"JS(
        encodeURIComponent('a b&c') + '|' + decodeURIComponent('a%20b') + '|' + atob(btoa('hi!'));
    )JS", "\"a%20b%26c|a b|hi!\""));

    EXPECT_TRUE(Eval(v, "structured-clone", R"JS(
        var src = { a: 1, nested: { b: [2, 3] } };
        var c = structuredClone(src);
        c.nested.b.push(4);
        '' + c.nested.b.length + ':' + src.nested.b.length + ':' + (c !== src);
    )JS", "\"3:2:true\""));
}

TEST(DiscordPhase5, AsyncAndPlatform) {
    View v;
    ASSERT_TRUE(v.load_html(kShell, "https://discord.com/app"));

    v.eval_js(R"JS(
        globalThis.settled = '';
        Promise.allSettled([Promise.resolve(1), Promise.reject('e')]).then(function(rs){
            globalThis.settled = rs[0].status + ':' + rs[0].value + ',' + rs[1].status + ':' + rs[1].reason;
        });
    )JS");
    v.run_tasks();
    EXPECT_TRUE(Eval(v, "promise-allSettled", "globalThis.settled", "\"fulfilled:1,rejected:e\""));

    EXPECT_TRUE(Eval(v, "date", R"JS(
        var d = new Date(0);
        d.getTime() + '|' + d.getFullYear() + '|' + d.toISOString();
    )JS", "\"0|1970|1970-01-01T00:00:00.000Z\""));

    EXPECT_TRUE(Eval(v, "performance-now", "typeof performance.now()", "\"number\""));
    EXPECT_TRUE(Eval(v, "crypto-uuid", "crypto.randomUUID().length", "36"));

    EXPECT_TRUE(Eval(v, "url", R"JS(
        var u = new URL('https://discord.com/api/v9/users/@me?limit=50#frag');
        u.protocol + '|' + u.hostname + '|' + u.pathname + '|' + u.search + '|' + u.hash + '|' + u.origin;
    )JS", "\"https:|discord.com|/api/v9/users/@me|?limit=50|#frag|https://discord.com\""));

    EXPECT_TRUE(Eval(v, "urlsearchparams", R"JS(
        var p = new URLSearchParams('a=1&b=2&b=3');
        p.get('a') + '|' + p.has('b') + '|' + p.get('b');
    )JS", "\"1|true|2\""));

    EXPECT_TRUE(Eval(v, "location-navigator", R"JS(
        '' + (location.hostname) + '|' + (typeof navigator.userAgent) + '|' + (window === self) + '|' + (window.location === location);
    )JS", "\"discord.com|string|true|true\""));
}
