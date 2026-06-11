// tests/test_discord_phase3.cpp
// Phase 3 of "run Discord": deeper JS-language correctness that React/Redux-style
// code depends on — arrow lexical `this`, class getters/setters, common Array/
// String methods, and regex. Surfaces the next concrete gaps.

#include <gtest/gtest.h>
#include "malibu/view/view.h"

using malibu::view::View;

namespace {
constexpr const char* kShell =
    "<!doctype html><html><body><div id='app-mount'></div></body></html>";

::testing::AssertionResult Eval(View& v, const char* label, const std::string& src,
                                const std::string& expected) {
    std::string r = v.eval_js(src);
    if (r == expected) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << "Discord phase3 [" << label << "]: got " << r << ", expected " << expected;
}
}  // namespace

// Arrow functions capture `this` lexically — React class methods as fields, and
// callbacks passed to map/forEach/then.
TEST(DiscordPhase3, ArrowLexicalThis) {
    View v;
    ASSERT_TRUE(v.load_html(kShell, "https://discord.com/app"));

    EXPECT_TRUE(Eval(v, "arrow-this-in-method", R"JS(
        class C {
            constructor() { this.vals = [1, 2, 3]; this.factor = 10; }
            scaled() { return this.vals.map(function(x){ return x; }).map((x) => x * this.factor); }
        }
        new C().scaled().join(',');
    )JS", "\"10,20,30\""));

    EXPECT_TRUE(Eval(v, "arrow-field-as-callback", R"JS(
        class Btn {
            constructor() { this.name = 'ok'; }
            handler = () => this.name;
        }
        var b = new Btn();
        var detached = b.handler;   // called with no receiver
        detached();
    )JS", "\"ok\""));
}

// Class getters/setters — computed/derived props all over Discord's models.
TEST(DiscordPhase3, GettersSetters) {
    View v;
    ASSERT_TRUE(v.load_html(kShell, "https://discord.com/app"));
    EXPECT_TRUE(Eval(v, "class-get-set", R"JS(
        class Temp {
            constructor() { this._c = 0; }
            get celsius() { return this._c; }
            set celsius(v) { this._c = v; }
            get fahrenheit() { return this._c * 9 / 5 + 32; }
        }
        var t = new Temp();
        t.celsius = 100;
        t.celsius + ':' + t.fahrenheit;
    )JS", "\"100:212\""));
}

// Common Array / String / Object methods used in render + state code.
TEST(DiscordPhase3, CommonMethods) {
    View v;
    ASSERT_TRUE(v.load_html(kShell, "https://discord.com/app"));
    EXPECT_TRUE(Eval(v, "array-methods", R"JS(
        var xs = [1, 2, 3, 4, 5];
        '' + xs.find(x => x > 3) + ':' + xs.includes(4) + ':' +
             xs.filter(x => x % 2).reduce((a, b) => a + b, 0) + ':' +
             xs.findIndex(x => x === 3);
    )JS", "\"4:true:9:2\""));
    EXPECT_TRUE(Eval(v, "object-entries", R"JS(
        var o = { a: 1, b: 2 };
        Object.entries(o).map(e => e[0] + '=' + e[1]).join('&');
    )JS", "\"a=1&b=2\""));
}

// Regex literals — routing, validation, parsing.
TEST(DiscordPhase3, Regex) {
    View v;
    ASSERT_TRUE(v.load_html(kShell, "https://discord.com/app"));
    EXPECT_TRUE(Eval(v, "regex-test-replace", R"JS(
        var re = /\d+/;
        '' + re.test('abc123') + ':' + 'a1b2c3'.replace(/\d/g, '#');
    )JS", "\"true:a#b#c#\""));
}
