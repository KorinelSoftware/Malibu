// tests/test_discord.cpp
// Empirical "run Discord" harness. Rather than implementing the whole web from
// memory, we boot a Discord-SHAPED client inside a real MalibuView and let the
// engine tell us the next missing API. Each probe mirrors something Discord's
// actual web client does at startup (webpack runtime, React-like ES6 classes,
// Map/Set, fetch, WebSocket, localStorage). The first failing probe is the next
// thing to implement.
//
// Method (per the user): run -> observe error #N -> implement missing API ->
// repeat. Probes assert EXACT values so a wrong-but-non-throwing result is also
// caught, not just crashes.

#include <gtest/gtest.h>
#include "malibu/view/view.h"

using malibu::view::View;

namespace {

// A pixel-thin shell that looks like Discord's index.html mount point.
constexpr const char* kShell = R"HTML(
<!doctype html>
<html><head><title>Discord</title></head>
<body>
  <div id="app-mount"></div>
</body></html>
)HTML";

// Assert that evaluating `src` yields exactly `expected` (JSON-serialized).
::testing::AssertionResult Eval(View& v, const char* label, const std::string& src,
                                const std::string& expected) {
    std::string r = v.eval_js(src);
    if (r == expected) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << "Discord boot [" << label << "]: got " << r << ", expected " << expected;
}

}  // namespace

// ---------------------------------------------------------------------------
// Phase 1: the web client's language + module system must parse and run.
// ---------------------------------------------------------------------------
TEST(Discord, BootstrapLanguageAndModuleSystem) {
    View v;
    ASSERT_TRUE(v.load_html(kShell, "https://discord.com/app"));

    // ES6 class with inheritance + super — every React component.
    EXPECT_TRUE(Eval(v, "es6-class", R"JS(
        class Component { constructor(props) { this.props = props; }
                          render() { return 'base'; } }
        class App extends Component {
            constructor(p) { super(p); this.state = { ready: true }; }
            render() { return 'app:' + this.props.name + ':' + this.state.ready; } }
        new App({ name: 'discord' }).render();
    )JS", "\"app:discord:true\""));

    // Static members + fields + inherited statics.
    EXPECT_TRUE(Eval(v, "class-static-fields", R"JS(
        class Base { static kind() { return 'base'; } }
        class Store extends Base {
            static NAME = 'store';
            count = 0;
            increment() { this.count++; return this.count; }
        }
        var s = new Store(); s.increment(); s.increment();
        '' + Store.NAME + ':' + Store.kind() + ':' + s.count;
    )JS", "\"store:base:2\""));

    // Array + object destructuring with defaults & rest — pervasive in bundles.
    EXPECT_TRUE(Eval(v, "destructuring", R"JS(
        var [a, b, ...rest] = [1, 2, 3, 4];
        var { x, y = 10, ...others } = { x: 1, z: 9, w: 8 };
        a + b + rest.length + x + y + others.z;
    )JS", "25"));

    // Destructuring in function parameters (React render props, redux).
    EXPECT_TRUE(Eval(v, "param-destructuring", R"JS(
        function render({ title, count = 1 }, ...tags) {
            return title + ':' + count + ':' + tags.join(',');
        }
        render({ title: 'msg' }, 'a', 'b');
    )JS", "\"msg:1:a,b\""));

    // Spread in calls and array/object literals — webpack + redux.
    EXPECT_TRUE(Eval(v, "spread", R"JS(
        function sum() { var t = 0; for (var i=0;i<arguments.length;i++) t+=arguments[i]; return t; }
        var nums = [1, 2, 3];
        var clone = { ...{ a: 1, b: 2 }, c: 3 };
        sum(...nums) + clone.a + clone.b + clone.c;
    )JS", "12"));

    // Map / Set — Discord's stores and iterables.
    EXPECT_TRUE(Eval(v, "collections", R"JS(
        var m = new Map(); m.set('a', 1).set('b', 2);
        var s = new Set([1, 1, 2, 3]);
        var total = 0; m.forEach(function(v){ total += v; });
        m.get('a') + m.get('b') + m.size + s.size + total + (s.has(2) ? 100 : 0);
    )JS", "111"));

    // Function.prototype.call/apply/bind — the webpack module trampoline.
    EXPECT_TRUE(Eval(v, "fn-call-apply-bind", R"JS(
        function greet(p) { return this.name + p; }
        var ctx = { name: 'hi' };
        greet.call(ctx, '!') + '|' + greet.apply(ctx, ['?']) + '|' + greet.bind(ctx)('.');
    )JS", "\"hi!|hi?|hi.\""));

    // A webpack-style chunk push: array-of-functions registry + require + .call.
    EXPECT_TRUE(Eval(v, "webpack-runtime", R"JS(
        (function(modules) {
            var cache = {};
            function require(id) {
                if (cache[id]) return cache[id].exports;
                var m = cache[id] = { exports: {} };
                modules[id].call(m.exports, m, m.exports, require);
                return m.exports;
            }
            globalThis.__wp = require(0);
        })([
            function(module, exports, require) { exports.value = 42; }
        ]);
        globalThis.__wp.value;
    )JS", "42"));
}
