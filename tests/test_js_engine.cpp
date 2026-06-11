// tests/test_js_engine.cpp
// End-to-end MalibuJS: source -> parse -> compile -> execute (Tasks 14-18).

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
std::string run(const char* src) { Engine e; return e.eval_to_string(src); }
}  // namespace

TEST(JSEngine, ArithmeticAndPrecedence) {
    EXPECT_EQ(run("1 + 2 * 3"), "7");
    EXPECT_EQ(run("(1 + 2) * 3"), "9");
    EXPECT_EQ(run("2 ** 10"), "1024");
    EXPECT_EQ(run("17 % 5"), "2");
    EXPECT_EQ(run("10 / 4"), "2.5");
    EXPECT_EQ(run("-(3 + 4)"), "-7");
    EXPECT_EQ(run("5e-324 > 0"), "true");
}

TEST(JSEngine, VariablesAndAssignment) {
    EXPECT_EQ(run("let x = 5; x += 3; x *= 2; x"), "16");
    EXPECT_EQ(run("const a = 10; const b = 20; a + b"), "30");
    EXPECT_EQ(run("let i = 0; i++; i++; i"), "2");
    EXPECT_EQ(run("let a = 0; let b = 0; (a = 4, b = 5, a + b)"), "9");
    EXPECT_EQ(run(
        "const runtime = {};"
        "runtime.a = 1, runtime.n = 2, runtime.O = () => 42;"
        "runtime.O()"),
        "42");
}

TEST(JSEngine, Strings) {
    EXPECT_EQ(run("'Hola' + ' ' + 'Malibu'"), "Hola Malibu");
    EXPECT_EQ(run("'abc'.length"), "3");
    EXPECT_EQ(run("'Malibu'.toUpperCase()"), "MALIBU");
    EXPECT_EQ(run("'a,b,c'.split(',').length"), "3");
    EXPECT_EQ(run("'hello world'.slice(0, 5)"), "hello");
    EXPECT_EQ(run("'abc'.includes('b')"), "true");
}

TEST(JSEngine, TaggedTemplatesPreserveCookedAndRawSegments) {
    EXPECT_EQ(
        run("function tag(parts, value) {"
            "  return parts[0] + '|' + parts.raw[0] + '|' + value + '|' +"
            "         parts[1] + '|' + parts.raw[1];"
            "}"
            "tag`line\\n${3}tail`"),
        "line\n|line\\n|3|tail|tail");
    EXPECT_EQ(run("String.raw`\\p{Emoji}${'-'}\\d`"), "\\p{Emoji}-\\d");
}

TEST(JSEngine, FunctionsAndRecursion) {
    EXPECT_EQ(run("function fact(n){ return n <= 1 ? 1 : n * fact(n - 1); } fact(5)"), "120");
    EXPECT_EQ(run("function fib(n){ return n < 2 ? n : fib(n-1) + fib(n-2); } fib(10)"), "55");
    EXPECT_EQ(run("const add = (a, b) => a + b; add(3, 4)"), "7");
}

TEST(JSEngine, Closures) {
    EXPECT_EQ(run(
        "function makeCounter(){ let c = 0; return function(){ c = c + 1; return c; }; }"
        "const inc = makeCounter(); inc(); inc(); inc()"), "3");
    EXPECT_EQ(run(
        "function adder(x){ return (y) => x + y; }"
        "const add10 = adder(10); add10(5)"), "15");
}

TEST(JSEngine, ArraysAndHigherOrder) {
    EXPECT_EQ(run("[1,2,3,4].map(x => x * x).join(',')"), "1,4,9,16");
    EXPECT_EQ(run("[1,2,3,4,5,6].filter(x => x % 2 === 0).join(',')"), "2,4,6");
    EXPECT_EQ(run("[1,2,3,4,5].reduce((a, b) => a + b, 0)"), "15");
    EXPECT_EQ(run("let a = [1,2]; a.push(3); a.push(4); a.length"), "4");
    EXPECT_EQ(run("[3,1,2].indexOf(2)"), "2");
    EXPECT_EQ(run("[1,2,3].includes(2)"), "true");
}

TEST(JSEngine, LargeArrayLiteralDoesNotExhaustRegisters) {
    std::string source = "[";
    for (int i = 0; i < 512; ++i) {
        if (i != 0) source += ",";
        source += std::to_string(i);
    }
    source += "].length";

    Engine engine;
    EXPECT_EQ(engine.eval_to_string(source), "512");
}

TEST(JSEngine, NestedArrayLiteralDoesNotExhaustRegisters) {
    std::string source = "[";
    for (int i = 0; i < 254; ++i) {
        if (i != 0) source += ",";
        source += "{values:[" + std::to_string(i) + "]}";
    }
    source += "][253].values[0]";

    Engine engine;
    EXPECT_EQ(engine.eval_to_string(source), "253");
}

TEST(JSEngine, WideCallsAndConstructorsUseArgumentVectors) {
    std::string args;
    for (int i = 0; i < 279; ++i) {
        if (i != 0) args += ",";
        args += std::to_string(i);
    }

    Engine engine;
    EXPECT_EQ(engine.eval_to_string(
                  "function count(){return arguments.length;} count(" + args + ")"),
              "279");
    EXPECT_EQ(engine.eval_to_string(
                  "function Count(){this.value=arguments.length;}"
                  "(new Count(" + args + ")).value"),
              "279");
}

TEST(JSEngine, Objects) {
    EXPECT_EQ(run("const o = { a: 1, b: 2 }; o.a + o.b"), "3");
    EXPECT_EQ(run("const o = {}; o.x = 42; o.x"), "42");
    EXPECT_EQ(run("const o = { name: 'Malibu', greet(){ return 'hi ' + this.name; } }; o.greet()"), "hi Malibu");
    EXPECT_EQ(run("Object.keys({a:1, b:2, c:3}).length"), "3");
    EXPECT_EQ(run("const o = {a:1}; 'a' in o"), "true");
}

TEST(JSEngine, ControlFlow) {
    EXPECT_EQ(run("let s = 0; for (let i = 1; i <= 10; i++) s += i; s"), "55");
    EXPECT_EQ(run("let s = 0; let i = 0; while (i < 5) { s += i; i++; } s"), "10");
    EXPECT_EQ(run("let i = 0; do; while (i++ < 2); i"), "3");
    EXPECT_EQ(run("let s = 0; for (const x of [10, 20, 30]) s += x; s"), "60");
    EXPECT_EQ(run("let r = ''; for (let i = 0; i < 5; i++){ if (i === 2) continue; if (i === 4) break; r += i; } r"), "013");
    EXPECT_EQ(run("let x = 7; x > 5 ? 'big' : 'small'"), "big");
    EXPECT_EQ(run(
        "let result = 'running';"
        "outer: for (let i = 0;"
        "  (function(){ while(false){} return i < 1; })();"
        "  i++) {"
        "  for (;;) { result = 'done'; break outer; }"
        "}"
        "result"),
        "done");
}

TEST(JSEngine, LogicalAndNullish) {
    EXPECT_EQ(run("true && 'yes'"), "yes");
    EXPECT_EQ(run("false || 'fallback'"), "fallback");
    EXPECT_EQ(run("null ?? 'default'"), "default");
    EXPECT_EQ(run("0 ?? 'default'"), "0");
    EXPECT_EQ(run("undefined ?? 42"), "42");
}

TEST(JSEngine, OptionalChainsShortCircuitAsAUnit) {
    EXPECT_EQ(run("let source; source?.app.getBuildNumber()"), "undefined");
    EXPECT_EQ(run(
        "let source; let calls = 0;"
        "source?.items[calls++].run(calls++);"
        "calls"),
        "0");
    EXPECT_EQ(run(
        "const source = { value: 7, get(){ return this.value; } };"
        "source?.get()"),
        "7");
    EXPECT_EQ(run(
        "let source; let threw = false;"
        "try { (source?.app).getBuildNumber(); } catch (e) { threw = true; }"
        "threw"),
        "true");
    EXPECT_EQ(run(
        "const source = { app: undefined }; let threw = false;"
        "try { source?.app.getBuildNumber(); } catch (e) { threw = true; }"
        "threw"),
        "true");
}

TEST(JSEngine, TryCatchThrow) {
    EXPECT_EQ(run(
        "let result;"
        "try { throw 'boom'; } catch (e) { result = 'caught ' + e; }"
        "result"), "caught boom");
    EXPECT_EQ(run(
        "function risky(){ throw new Error('bad'); }"
        "let msg; try { risky(); } catch (e) { msg = e.message; } msg"), "bad");
    EXPECT_EQ(run("String(new Error('bad'))"), "Error: bad");
}

TEST(JSEngine, TryFinallyRuns) {
    EXPECT_EQ(run(
        "let log = '';"
        "try { log += 'a'; } finally { log += 'b'; }"
        "log"), "ab");
}

TEST(JSEngine, Typeof) {
    EXPECT_EQ(run("typeof 42"), "number");
    EXPECT_EQ(run("typeof 'x'"), "string");
    EXPECT_EQ(run("typeof true"), "boolean");
    EXPECT_EQ(run("typeof undefined"), "undefined");
    EXPECT_EQ(run("typeof function(){}"), "function");
    EXPECT_EQ(run("typeof {}"), "object");
}

TEST(JSEngine, MathAndJson) {
    EXPECT_EQ(run("Math.max(3, 7, 2)"), "7");
    EXPECT_EQ(run("Math.floor(3.9)"), "3");
    EXPECT_EQ(run("Math.abs(-5)"), "5");
    EXPECT_EQ(run("JSON.stringify({a:1, b:[2,3]})"), "{\"a\":1,\"b\":[2,3]}");
}

TEST(JSEngine, ConsoleLogCaptured) {
    Engine e;
    CaptureSink sink;
    e.set_console_sink(&sink);
    auto r = e.evaluate("console.log('hello', 42, true);");
    EXPECT_TRUE(r.ok) << r.error;
    ASSERT_EQ(sink.lines.size(), 1u);
    EXPECT_EQ(sink.lines[0], "hello 42 true");
}

TEST(JSEngine, NewConstructor) {
    EXPECT_EQ(run(
        "function Point(x, y){ this.x = x; this.y = y; }"
        "const p = new Point(3, 4); p.x + p.y"), "7");
    EXPECT_EQ(run(
        "function Animal(name){ this.name = name; }"
        "Animal.prototype.speak = function(){ return this.name + ' speaks'; };"
        "const a = new Animal('Rex'); a.speak()"), "Rex speaks");
}

TEST(JSEngine, ParseErrorReported) {
    Engine e;
    auto r = e.evaluate("let x = ;", "bad.js");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("bad.js"), std::string::npos);
}

TEST(JSEngine, RuntimeErrorReported) {
    Engine e;
    auto r = e.evaluate("undefinedFunction();");
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("not defined"), std::string::npos);
}

TEST(JSEngine, GarbageCollectionSurvivesReachable) {
    Engine e;
    // Allocate a lot, then force a collection; reachable data must survive.
    auto r = e.evaluate(
        "let keep = [];"
        "for (let i = 0; i < 500; i++){ keep.push({ id: i, s: 'x' + i }); }"
        "keep.length");
    ASSERT_TRUE(r.ok) << r.error;
    e.heap().collect();
    EXPECT_EQ(e.interpreter().to_string(r.value), u"500");
}
