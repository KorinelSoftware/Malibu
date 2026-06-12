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

TEST(JSEngine, BigIntUsesArbitraryPrecision) {
    EXPECT_EQ(run("typeof BigInt"), "function");
    EXPECT_EQ(run("typeof 1n"), "bigint");
    EXPECT_EQ(
        run("(123456789012345678901234567890n + 10n).toString()"),
        "123456789012345678901234567900");
    EXPECT_EQ(run("(2n ** 100n).toString()"), "1267650600228229401496703205376");
    EXPECT_EQ(run("0xffn.toString(16)"), "ff");
    EXPECT_EQ(run("typeof 0xffn"), "bigint");
    EXPECT_EQ(run("1_000_000n.toString()"), "1000000");
}

TEST(JSEngine, BigIntArithmeticAndBitOperations) {
    EXPECT_EQ(
        run("[7n/3n,-7n/3n,-7n%3n,~0n,1n<<65n,-8n>>2n].join('|')"),
        "2|-2|-1|-1|36893488147419103232|-2");
    EXPECT_EQ(
        run("[BigInt.asUintN(8,-1n),BigInt.asIntN(8,255n),"
            "BigInt.asIntN(0,123n)].join('|')"),
        "255|-1|0");
}

TEST(JSEngine, BigIntConversionsComparisonsAndIncrement) {
    EXPECT_EQ(
        run("let x=1n; x++; ++x;"
            "[x,1n===BigInt('1'),1n==1,1n===1,"
            "9007199254740993n>9007199254740992n,"
            "Object.prototype.toString.call(1n)].join('|')"),
        "3|true|true|false|true|[object BigInt]");
    EXPECT_EQ(run("Number(9007199254740993n)"), "9.00719925474099e+15");
}

TEST(JSEngine, BigIntRejectsInvalidOperations) {
    EXPECT_EQ(
        run("let out=[];"
            "for (let f of [()=>1n+1,()=>new BigInt(1),()=>1n>>>1n,"
            "()=>JSON.stringify(1n),()=>BigInt(1.5)]) {"
            "  try { f(); out.push('bad'); } catch(e) { out.push(e.name); }"
            "}"
            "out.join('|')"),
        "TypeError|TypeError|TypeError|TypeError|RangeError");
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

TEST(JSEngine, MemberAssignmentsEvaluateTheReferenceBeforeTheRhs) {
    EXPECT_EQ(run(
        "const initial={theme:'dark'};"
        "function createContext(value){"
        "  return (value={"
        "    $$typeof:Symbol.for('react.context'),"
        "    _currentValue:value,"
        "    Provider:null"
        "  }).Provider=value,value;"
        "}"
        "const context=createContext(initial);"
        "let calls=[];"
        "let holder={value:1};"
        "function base(){calls.push('base');return holder;}"
        "function key(){calls.push('key');return 'value';}"
        "function rhs(){calls.push('rhs');return 4;}"
        "base()[key()]+=rhs();"
        "base()[key()]&&=rhs();"
        "[context.Provider===context,"
        " context._currentValue===initial,"
        " holder.value,calls.join(',')].join('|')"),
        "true|true|4|base,key,rhs,base,key,rhs");
}

TEST(JSEngine, Strings) {
    EXPECT_EQ(run("'Hola' + ' ' + 'Malibu'"), "Hola Malibu");
    EXPECT_EQ(run("'abc'.length"), "3");
    EXPECT_EQ(run("'Malibu'.toUpperCase()"), "MALIBU");
    EXPECT_EQ(run("'a,b,c'.split(',').length"), "3");
    EXPECT_EQ(run("'hello world'.slice(0, 5)"), "hello");
    EXPECT_EQ(run("'abc'.includes('b')"), "true");
    EXPECT_EQ(run(
        "['abcdef'.substr(1, 3), 'abcdef'.substr(-2),"
        " 'abcdef'.substr(2, -1)].join('|')"),
        "bcd|ef|");
    EXPECT_EQ(
        run(
            "["
            " ''.substr(1)==='',"
            " ''.substr(Infinity)==='',"
            " 'abc'.substr(Infinity)==='',"
            " 'abc'.substr(-Infinity)==='abc',"
            " 'abc'.substr(1,Infinity)==='bc',"
            " 'abc'.slice(Infinity)==='',"
            " 'abc'.slice(-Infinity)==='abc',"
            " 'abc'.slice(4294967297)==='',"
            " 'abc'.substring(Infinity)==='',"
            " 'abc'.substring(-Infinity)==='abc',"
            " 'abc'.substring(3,1)==='bc'"
            "].join('|')"),
        "true|true|true|true|true|true|true|true|true|true|true");
    EXPECT_EQ(
        run(
            "["
            " ''.replaceAll(/(?:)/g,'x'),"
            " 'ab'.replaceAll(/(?:)/g,'-'),"
            " 'ab'.replace('', '_'),"
            " 'ab'.replaceAll('', '_')"
            "].join('|')"),
        "x|-a-b-|_ab|_a_b_");
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

TEST(JSEngine, NamedFunctionExpressionsUseTheirPrivateRecursiveBinding) {
    EXPECT_EQ(
        run(
            "let visit = { notCallable: true };"
            "const factorial = function visit(value) {"
            "  return value < 2 ? 1 : value * visit(value - 1);"
            "};"
            "[factorial(5), typeof visit].join('|')"),
        "120|object");
}

TEST(JSEngine, Closures) {
    EXPECT_EQ(run(
        "function makeCounter(){ let c = 0; return function(){ c = c + 1; return c; }; }"
        "const inc = makeCounter(); inc(); inc(); inc()"), "3");
    EXPECT_EQ(run(
        "function adder(x){ return (y) => x + y; }"
        "const add10 = adder(10); add10(5)"), "15");
}

TEST(JSEngine, ClosuresObserveLaterDeclaratorsInTheSameScope) {
    EXPECT_EQ(run(
        "let exported;"
        "(function(){"
        "  const getters = { Ay: () => K };"
        "  exported = getters.Ay;"
        "  let W = { value: 7 }, K = W;"
        "})();"
        "exported().value"),
        "7");
}

TEST(JSEngine, ClassConstructorsBindDefaultDestructuredAndRestParameters) {
    EXPECT_EQ(
        run("let value = { outer: true };"
            "class Store {"
            "  ready = 'yes';"
            "  constructor(value = 2, { count = 3 } = {}, ...rest) {"
            "    this.result = [value, count, rest.length, this.ready].join('|');"
            "  }"
            "}"
            "const store = new Store(undefined, undefined, 1, 2);"
            "[Store.length, store.result].join('|')"),
        "0|2|3|2|yes");
}

TEST(JSEngine, CapturedLexicalEnvironmentSurvivesCollection) {
    Engine e;
    auto setup = e.evaluate(
        "let exported;"
        "(function(){"
        "  const getters = { Ay: () => K };"
        "  exported = getters.Ay;"
        "  let W = { value: 7 }, K = W;"
        "})();");
    ASSERT_TRUE(setup.ok) << setup.error;

    e.heap().collect();

    auto observed = e.evaluate("exported().value");
    ASSERT_TRUE(observed.ok) << observed.error;
    EXPECT_EQ(e.interpreter().to_string(observed.value), u"7");
}

TEST(JSEngine, ModuleBindingsDoNotCorruptClassicScriptClosures) {
    Engine engine;
    auto setup = engine.evaluate(
        "class e { constructor(){ this.source='classic'; } }"
        "class Loader { load(){ return new e().source; } }"
        "globalThis.loader = new Loader();");
    ASSERT_TRUE(setup.ok) << setup.error;

    auto module = engine.evaluate_module(
        "class e { constructor(){ this.source='module'; } }"
        "globalThis.moduleBindingVisible = typeof e;");
    ASSERT_TRUE(module.ok) << module.error;

    EXPECT_EQ(engine.eval_to_string("loader.load()"), "classic");
    EXPECT_EQ(engine.eval_to_string("typeof e"), "function");
    EXPECT_EQ(engine.eval_to_string("moduleBindingVisible"), "function");
}

TEST(JSEngine, ArraysAndHigherOrder) {
    EXPECT_EQ(run("[1,2,3,4].map(x => x * x).join(',')"), "1,4,9,16");
    EXPECT_EQ(run("[1,2,3,4,5,6].filter(x => x % 2 === 0).join(',')"), "2,4,6");
    EXPECT_EQ(run("[1,2,3,4,5].reduce((a, b) => a + b, 0)"), "15");
    EXPECT_EQ(run("let a = [1,2]; a.push(3); a.push(4); a.length"), "4");
    EXPECT_EQ(run("[3,1,2].indexOf(2)"), "2");
    EXPECT_EQ(run("[1,2,3].includes(2)"), "true");
}

TEST(JSEngine, SparseArraysPreserveElisionsAndObservableHoles) {
    EXPECT_EQ(
        run(
            "const modules = [, () => 1, () => 2];"
            "let calls = 0;"
            "const mapped = modules.map((fn) => { calls++; return fn && fn(); });"
            "["
            " modules.length,"
            " typeof modules[0],"
            " modules[1](),"
            " modules[2](),"
            " 0 in modules,"
            " Object.keys(modules).join(','),"
            " calls,"
            " 0 in mapped,"
            " mapped.length"
            "].join('|')"),
        "3|undefined|1|2|false|1,2|2|false|3");
    EXPECT_EQ(run("let a = new Array(2); a[1] = 7; delete a[1];"
                  "[a.length, a.hasOwnProperty(0), a.hasOwnProperty(1)].join('|')"),
              "2|false|false");
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

TEST(JSEngine, SymbolsAreUniquePrimitivesWithAStableGlobalRegistry) {
    EXPECT_EQ(
        run(
            "const a=Symbol('x'),b=Symbol('x');"
            "const fragment=Symbol.for('react.fragment');"
            "const sameFragment=Symbol.for('react.fragment');"
            "const object={};object[Symbol.iterator]=7;"
            "let constructorThrows=false;"
            "try{new Symbol()}catch(error){"
            " constructorThrows=error instanceof TypeError;"
            "}"
            "[typeof a,a!==b,fragment===sameFragment,"
            " String(fragment),Symbol.keyFor(fragment),"
            " Symbol.keyFor(a)===undefined,"
            " object[Symbol.iterator],Object.keys(object).length,"
            " Symbol.iterator===Symbol.iterator,constructorThrows].join('|')"),
        "symbol|true|true|Symbol(react.fragment)|react.fragment|true|"
        "7|0|true|true");
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
    EXPECT_EQ(
        run("let visited='';"
            "function cleanup(stop){if(stop)return\n"
            "for(const key of ['a','b'])visited+=key}"
            "cleanup(true);cleanup(false);visited"),
        "ab");
}

TEST(JSEngine, OptionalSuperMethodCallsPreserveTheDerivedReceiver) {
    EXPECT_EQ(
        run("class Base{ping(){return this.value}}"
            "class Child extends Base{"
            " constructor(){super();this.value=8}"
            " ping(){return super.ping?.()+1}"
            " missing(){return super.missing?.()}"
            "}"
            "const child=new Child();"
            "[child.ping(),child.missing()].join('|')"),
        "9|");
}

TEST(JSEngine, DestructuringAssignmentsSkipArrayHoles) {
    EXPECT_EQ(
        run("let first=0,second=0;"
            "[,first,second]=[1,7,9];"
            "[first,second].join('|')"),
        "7|9");
}

TEST(JSEngine, SuperPropertyAssignmentsUseTheDerivedReceiver) {
    EXPECT_EQ(
        run("class Base{"
            " get pageIndex(){return this.saved||0}"
            " set pageIndex(value){this.saved=value}"
            "}"
            "class Child extends Base{"
            " setPage(value){super.pageIndex=value}"
            " increment(){super.pageIndex+=2}"
            " getPage(){return super.pageIndex}"
            "}"
            "const child=new Child();"
            "child.setPage(5);child.increment();"
            "[child.saved,child.getPage()].join('|')"),
        "7|7");
}

TEST(JSEngine, LogicalAndNullish) {
    EXPECT_EQ(run("true && 'yes'"), "yes");
    EXPECT_EQ(run("false || 'fallback'"), "fallback");
    EXPECT_EQ(run("null ?? 'default'"), "default");
    EXPECT_EQ(run("0 ?? 'default'"), "0");
    EXPECT_EQ(run("undefined ?? 42"), "42");
}

TEST(JSEngine, AbstractEqualityDoesNotCoerceObjectsAgainstNullish) {
    EXPECT_EQ(run(
        "let conversions = 0;"
        "function target() {}"
        "target.valueOf = function() { conversions++; return null; };"
        "[null == target, undefined == target, conversions,"
        " false == '0', [] == 0].join('|')"),
        "false|false|0|true|true");
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

TEST(JSEngine, FinallyRunsForAbruptFunctionCompletion) {
    EXPECT_EQ(
        run("let log = '';"
            "function returnsFromTry() {"
            "  try { log += 'a'; return 'value'; }"
            "  finally { log += 'b'; }"
            "}"
            "[returnsFromTry(), log].join('|')"),
        "value|ab");
    EXPECT_EQ(
        run("function overridesReturn() {"
            "  try { return 1; } finally { return 2; }"
            "}"
            "overridesReturn()"),
        "2");
    EXPECT_EQ(
        run("let log = '';"
            "function returnsFromCatch() {"
            "  try { throw 'x'; }"
            "  catch (e) { return e; }"
            "  finally { log += 'f'; }"
            "}"
            "[returnsFromCatch(), log].join('|')"),
        "x|f");
    EXPECT_EQ(
        run("let log = ''; let caught = '';"
            "try {"
            "  try { throw 'first'; }"
            "  catch (e) { throw 'second'; }"
            "  finally { log += 'f'; }"
            "} catch (e) { caught = e; }"
            "[caught, log].join('|')"),
        "second|f");
}

TEST(JSEngine, Typeof) {
    EXPECT_EQ(run("typeof 42"), "number");
    EXPECT_EQ(run("typeof 'x'"), "string");
    EXPECT_EQ(run("typeof true"), "boolean");
    EXPECT_EQ(run("typeof undefined"), "undefined");
    EXPECT_EQ(run("typeof function(){}"), "function");
    EXPECT_EQ(run("typeof {}"), "object");
}

TEST(JSEngine, InOperatorSupportsFunctionsAndRejectsPrimitives) {
    EXPECT_EQ(
        run("['prototype' in String,"
            " 'indexOf' in String.prototype,"
            " 'call' in function(){}].join('|')"),
        "true|true|true");
    EXPECT_EQ(
        run("let name = '';"
            "try { 'x' in 1; } catch (e) { name = e.name; }"
            "name"),
        "TypeError");
}

TEST(JSEngine, ObjectToStringReportsBinaryDataBrands) {
    EXPECT_EQ(
        run("[new Uint8Array(1), new Float64Array(1),"
            " new ArrayBuffer(1), new DataView(new ArrayBuffer(1))]"
            ".map(value => Object.prototype.toString.call(value))"
            ".join('|')"),
        "[object Uint8Array]|[object Float64Array]|"
        "[object ArrayBuffer]|[object DataView]");
}

TEST(JSEngine, ObjectToStringAndCallBoundRecognizeRegExp) {
    EXPECT_EQ(
        run("const regex = /^\\s*(?:function)?\\*/;"
            "const callBindApply = Function.prototype.bind.call("
            "  Function.prototype.call, Function.prototype.apply);"
            "const callBound = fn => callBindApply("
            "  Function.prototype.bind, Function.prototype.call, [fn]);"
            "const exec = callBound(RegExp.prototype.exec);"
            "const sentinel = {};"
            "const fail = () => { throw sentinel; };"
            "const input = { toString: fail, valueOf: fail };"
            "input[Symbol.toPrimitive] = fail;"
            "let propagated = false;"
            "try { exec(regex, input); } catch (error) { propagated = error === sentinel; }"
            "[Object.prototype.toString.call(regex),"
            " Object.getOwnPropertyDescriptor(regex, 'lastIndex').value,"
            " propagated].join('|')"),
        "[object RegExp]|0|true");
}

TEST(JSEngine, URLSearchParamsUsesSharedPrototypeAndRepeatedValues) {
    EXPECT_EQ(
        run("const params = new URLSearchParams('a=1&a=2&b=3');"
            "params.delete('a', '1');"
            "params.append('a', '4');"
            "let seen = [];"
            "params.forEach((value, key) => seen.push(key + value));"
            "[params instanceof URLSearchParams,"
            " typeof URLSearchParams.prototype.getAll,"
            " params.getAll('a').join(','),"
            " params.has('a', '2'), seen.join(',')].join('|')"),
        "true|function|2,4|true|a2,b3,a4");
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
    auto r = e.evaluate(
        "console.log('hello', 42, true);"
        "console.debug('debug');"
        "console.assert(true, 'hidden');"
        "console.assert(false, 'asserted');"
        "console.group('group'); console.groupEnd();"
        "console.time('timer'); console.timeEnd('timer');");
    EXPECT_TRUE(r.ok) << r.error;
    ASSERT_EQ(sink.lines.size(), 3u);
    EXPECT_EQ(sink.lines[0], "hello 42 true");
    EXPECT_EQ(sink.lines[1], "debug");
    EXPECT_EQ(sink.lines[2], "asserted");
}

TEST(JSEngine, TranspiledEnumInitializerRetainsTheSharedObject) {
    EXPECT_EQ(
        run("(Alias = State || (State = {}))[Alias.PENDING = 0] = 'PENDING';"
            "Alias[Alias.RESOLVED = 1] = 'RESOLVED';"
            "class Deferred {"
            "  constructor() { this.state = State.PENDING; }"
            "}"
            "var Alias, State;"
            "const deferred = new Deferred();"
            "[deferred.state, State[0], Alias === State].join('|')"),
        "0|PENDING|true");
}

TEST(JSEngine, VarDeclarationInstantiationDoesNotResetValues) {
    EXPECT_EQ(
        run("const before = typeof value;"
            "value = 7;"
            "var value;"
            "[before, value].join('|')"),
        "undefined|7");
    EXPECT_EQ(
        run("function callback() { return 9; }"
            "var callback;"
            "callback()"),
        "9");
    EXPECT_EQ(
        run("const before = typeof first + '|' + typeof rest;"
            "var [first, ...rest] = [2, 3, 4];"
            "[before, first, rest.join(',')].join('|')"),
        "undefined|undefined|2|3,4");
    EXPECT_EQ(
        run("function preserve(value) {"
            "  if (value) return value;"
            "  var [value, other] = [1, 2];"
            "  return value + other;"
            "}"
            "[preserve(7), preserve(0)].join('|')"),
        "7|3");
}

TEST(JSEngine, NewConstructor) {
    EXPECT_EQ(run(
        "function Point(x, y){ this.x = x; this.y = y; }"
        "const p = new Point(3, 4); p.x + p.y"), "7");
    EXPECT_EQ(run(
        "function Animal(name){ this.name = name; }"
        "Animal.prototype.speak = function(){ return this.name + ' speaks'; };"
        "const a = new Animal('Rex'); a.speak()"), "Rex speaks");
    EXPECT_EQ(run(
        "function Box(value){ this.value = value; }"
        "const box = new Box(3);"
        "box instanceof Box"), "true");
    EXPECT_EQ(run(
        "function Box(value){ this.value = value; }"
        "Box.prototype.toJSNumber = Box.prototype.valueOf = function(){ return this.value; };"
        "const box = new Box(3);"
        "[box.valueOf(), box.toJSNumber(), box + 1].join('|')"), "3|3|4");
}

TEST(JSEngine, NewPreservesGroupedCallAsTheConstructorExpression) {
    EXPECT_EQ(run(
        "function Color(value) { this.value = value; }"
        "function create(args) {"
        "  return new(Function.prototype.bind.apply("
        "      Color, [null].concat(args)));"
        "}"
        "const fromFactory = new (function(){"
        "  return Color.bind(null, 8);"
        "}());"
        "const direct = new (Color)(9);"
        "[create([7]).value, fromFactory.value, direct.value].join('|')"),
        "7|8|9");
}

TEST(JSEngine, AnonymousFunctionsInferPropertyNames) {
    EXPECT_EQ(run(
        "const object = { literal: function(){} };"
        "object.assigned = function(){};"
        "object['computed'] = function(){};"
        "[object.literal.name, object.assigned.name, object.computed.name].join('|')"),
        "literal|assigned|computed");
    EXPECT_EQ(run(
        "const object = {};"
        "object.outer = object.inner = function(){};"
        "[object.outer.name, object.inner.name].join('|')"),
        "inner|inner");
    EXPECT_EQ(run(
        "const object = {};"
        "object.property = function explicit(){};"
        "object.property.name"),
        "explicit");
}

TEST(JSEngine, DefinePropertyStoresPrivateStateOnFunctions) {
    EXPECT_EQ(run(
        "const stateKey = 'Symbol(state)_3';"
        "function target(){}"
        "const state = { type: 'Function' };"
        "Object.defineProperty(target, stateKey, { value: state });"
        "["
        "  Object.prototype.hasOwnProperty.call(target, stateKey),"
        "  target[stateKey] === state,"
        "  Object.prototype.propertyIsEnumerable.call(target, stateKey),"
        "  Object.keys(target).indexOf(stateKey)"
        "].join('|')"),
        "true|true|false|-1");
}

TEST(JSEngine, OwnPropertyOperationsAcceptSymbolKeys) {
    EXPECT_EQ(
        run(
            "const key = Symbol('state');"
            "const object = {};"
            "Object.defineProperty(object, key, {"
            " value: 42, enumerable: true"
            "});"
            "["
            " Object.hasOwn(object, key),"
            " object.hasOwnProperty(key),"
            " object.propertyIsEnumerable(key),"
            " object[key],"
            " Symbol.toStringTag === Symbol.toStringTag"
            "].join('|')"),
        "true|true|true|42|true");
    EXPECT_EQ(
        run(
            "const key = Symbol('entry');"
            "const object = Object.fromEntries([[key, 7]]);"
            "[Object.hasOwn(object, key), object[key]].join('|')"),
        "true|7");
}

TEST(JSEngine, EnumerableAccessorDescriptorsSupportWebpackReexports) {
    EXPECT_EQ(
        run(
            "const source = {};"
            "Object.defineProperty(source, 'RuntimeLoader', {"
            " enumerable: true, get: () => 42"
            "});"
            "const target = {};"
            "Object.keys(source).forEach((key) => {"
            " Object.defineProperty(target, key, {"
            "  enumerable: true, get: () => source[key]"
            " });"
            "});"
            "[Object.keys(source).join(','), Object.keys(target).join(','),"
            " target.RuntimeLoader].join('|')"),
        "RuntimeLoader|RuntimeLoader|42");
}

TEST(JSEngine, ProxyInternalMethodsPreserveDiscordMessageObjects) {
    EXPECT_EQ(
        run(
            "function makeMessagesProxy(loader) {"
            " function bind(key) { return locale => loader.get(key, locale); }"
            " const base = {};"
            " const proxy = new Proxy(base, {"
            "  ownKeys: target => Reflect.ownKeys(target),"
            "  getOwnPropertyDescriptor: (target, key) => ("
            "   target[key] || (target[key] = bind(key)),"
            "   Reflect.getOwnPropertyDescriptor(target, key)"
            "  ),"
            "  get: (target, key) =>"
            "   key === Symbol.toStringTag ? 'IntlMessagesProxy' :"
            "   (target[key] || (target[key] = bind(key)), target[key])"
            " });"
            " Object.defineProperty(proxy, '$$baseObject', {"
            "  value: base, enumerable: false"
            " });"
            " Object.defineProperty(proxy, '$$loader', {"
            "  value: loader, enumerable: false"
            " });"
            " return proxy;"
            "}"
            "const fallbacks = [];"
            "const firstLoader = {"
            " get: (key, locale) => key + '@' + locale,"
            " fallbackWith: value => fallbacks.push(value)"
            "};"
            "const secondLoader = {get: () => 'second'};"
            "const first = makeMessagesProxy(firstLoader);"
            "const second = makeMessagesProxy(secondLoader);"
            "const isMessages = value =>"
            " value[Symbol.toStringTag] === 'IntlMessagesProxy';"
            "if (isMessages(first) && isMessages(second))"
            " first.$$loader.fallbackWith(second.$$loader);"
            "const descriptor ="
            " Object.getOwnPropertyDescriptor(first, 'welcome');"
            "["
            " isMessages(first),"
            " first.$$baseObject !== undefined,"
            " fallbacks[0] === secondLoader,"
            " descriptor.value('en-US'),"
            " Reflect.ownKeys(first).includes('welcome')"
            "].join('|')"),
        "true|true|true|welcome@en-US|true");
}

TEST(JSEngine, ObjectSpreadAndAssignReadEnumerableAccessors) {
    EXPECT_EQ(
        run(
            "let reads = 0;"
            "const source = {};"
            "Object.defineProperty(source, 'exported', {"
            " enumerable: true,"
            " get: () => { reads++; return function value(){ return 42; }; }"
            "});"
            "Object.defineProperty(source, 'hidden', {"
            " enumerable: false, value: 9"
            "});"
            "const spread = {...source};"
            "const assigned = Object.assign({}, source);"
            "const descriptor = Object.getOwnPropertyDescriptor("
            " spread, 'exported');"
            "[spread.exported(), assigned.exported(), reads,"
            " 'hidden' in spread, descriptor.value(),"
            " descriptor.enumerable].join('|')"),
        "42|42|2|false|42|true");
}

TEST(JSEngine, ArrayLengthHasARealPropertyDescriptor) {
    EXPECT_EQ(run(
        "const array = [1, 2];"
        "const before = Object.getOwnPropertyDescriptor(array, 'length');"
        "Object.defineProperty(array, 'length', { writable: false });"
        "array.length = 1;"
        "const after = Object.getOwnPropertyDescriptor(array, 'length');"
        "[before.value, before.writable, before.enumerable, before.configurable,"
        " array.length, after.writable].join('|')"),
        "2|true|false|false|2|false");
}

TEST(JSEngine, ArrayPushIsGenericAndHonorsLengthDescriptor) {
    EXPECT_EQ(run(
        "const object = { length: 0x100000000 };"
        "const length = [].push.call(object, 'value');"
        "[length, object.length, object[0x100000000]].join('|')"),
        "4294967297|4294967297|value");
    EXPECT_EQ(run(
        "const array = [];"
        "Object.defineProperty(array, 'length', { writable: false });"
        "let caught = false;"
        "try { array.push(); } catch (error) {"
        "  caught = error instanceof TypeError;"
        "}"
        "[caught, array.length, array[0]].join('|')"),
        "true|0|");
    EXPECT_EQ(run(
        "const array = [];"
        "Object.defineProperty(array, 'length', { writable: false });"
        "array[0] = 1;"
        "[array.length, array[0]].join('|')"),
        "0|");
}

TEST(JSEngine, NumericObjectKeysUseCanonicalPropertyNames) {
    EXPECT_EQ(run(
        "const modules = {"
        "  82e4(module) { module.exports = 7; },"
        "  0x10: 'hex',"
        "  1e3: 'decimal',"
        "  0b11n: 'bigint'"
        "};"
        "const module = { exports: 0 };"
        "modules[820000].call(module.exports, module);"
        "[module.exports, modules[16], modules[1000], modules[3]].join('|')"),
        "7|hex|decimal|bigint");
}

TEST(JSEngine, LocaleStringCaseMethodsSupportEntryMapping) {
    EXPECT_EQ(run(
        "const keyboard = { KeyA: 'A', KeyB: 'b' };"
        "const mapped = new Map(Object.entries(keyboard).map(entry => {"
        "  const [key, value] = entry;"
        "  return [key, value.toLocaleLowerCase()];"
        "}));"
        "[mapped.get('KeyA'), 'a'.toLocaleUpperCase(),"
        " 'a'.localeCompare('b')].join('|')"),
        "a|A|-1");
}

TEST(JSEngine, SetExposesStandardIterationMethods) {
    EXPECT_EQ(run(
        "const values = new Set(['a', 'b']);"
        "const entries = Array.from(values.entries());"
        "[values.keys === values.values,"
        " Array.from(values.keys()).join(','),"
        " Array.from(values).join(','),"
        " entries[1][0], entries[1][1]].join('|')"),
        "true|a,b|a,b|b|b");
}

TEST(JSEngine, MapSetAndStringExposeStandardIterators) {
    EXPECT_EQ(
        run(
            "const map = new Map([['a', 1], ['b', 2]]);"
            "const mapIterator = map[Symbol.iterator]();"
            "const first = mapIterator.next();"
            "const setIterator = new Set([3, 4]).values();"
            "const stringIterator = 'ABC'[Symbol.iterator]();"
            "["
            " first.value.join(':'), first.done,"
            " setIterator.next().value,"
            " stringIterator.next().value,"
            " stringIterator.next().value,"
            " mapIterator[Symbol.iterator]() === mapIterator"
            "].join('|')"),
        "a:1|false|3|A|B|true");
}

TEST(JSEngine, ArrayExposesLiveStandardIterators) {
    EXPECT_EQ(run(
        "const values = [10, 20];"
        "const iterator = values[Symbol.iterator]();"
        "const first = iterator.next();"
        "values.push(30);"
        "const second = iterator.next();"
        "const third = iterator.next();"
        "const done = iterator.next();"
        "const entries = Array.from(values.entries());"
        "[values[Symbol.iterator] === values.values,"
        " iterator[Symbol.iterator]() === iterator,"
        " first.value, first.done, second.value, third.value,"
        " done.done, Array.from(values.keys()).join(','),"
        " entries[2][0], entries[2][1]].join('|')"),
        "true|true|10|false|20|30|true|0,1,2|2|30");
}

TEST(JSEngine, NamedClassesBindThemselvesDuringInitialization) {
    EXPECT_EQ(run(
        "class DurationFormat {"
        "  static availableLocales = new Set(['en']);"
        "  static localeData = (() => {"
        "    DurationFormat.availableLocales.add('de');"
        "    return DurationFormat.availableLocales.size;"
        "  })();"
        "  constructor() { this.same = DurationFormat === this.constructor; }"
        "}"
        "const PublicName = class PrivateName {"
        "  static self = PrivateName;"
        "  method() { return PrivateName; }"
        "};"
        "const instance = new DurationFormat();"
        "const privateInstance = new PublicName();"
        "[DurationFormat.localeData, instance.same,"
        " PublicName.self === PublicName,"
        " privateInstance.method() === PublicName,"
        " typeof PrivateName].join('|')"),
        "2|true|true|true|undefined");
}

TEST(JSEngine, BabelClassFactoryConstructsWithTheReturnedPrototype) {
    EXPECT_EQ(run(
        "function classCallCheck(instance, Constructor) {"
        "  if (!(instance instanceof Constructor))"
        "    throw new TypeError('Cannot call a class as a function');"
        "}"
        "function createClass(Constructor) {"
        "  Object.defineProperty(Constructor, 'prototype', {writable:false});"
        "  return Constructor;"
        "}"
        "const instance = new (function(){"
        "  function Generated(){ classCallCheck(this, Generated); }"
        "  return createClass(Generated);"
        "}())();"
        "const Constructor = instance.constructor;"
        "const prototype = Constructor.prototype;"
        "Constructor.prototype = {};"
        "const descriptor = Object.getOwnPropertyDescriptor("
        "  Constructor, 'prototype');"
        "[Constructor.name,"
        " instance instanceof Constructor,"
        " Constructor.prototype === prototype,"
        " descriptor.writable,"
        " descriptor.configurable].join('|')"),
        "Generated|true|true|false|false");
}

TEST(JSEngine, IntlUsesIcuForModernFormattingPrimitives) {
    EXPECT_EQ(
        run(
            "const nf = new Intl.NumberFormat('de-DE', {"
            " style: 'unit', unit: 'degree', signDisplay: 'exceptZero'"
            "});"
            "const dtf = new Intl.DateTimeFormat('en-US');"
            "const pr = new Intl.PluralRules('en-US');"
            "const lf = new Intl.ListFormat('en-US');"
            "["
            " nf.resolvedOptions().style,"
            " nf.resolvedOptions().unit,"
            " typeof nf.format(12.5),"
            " nf.formatToParts(2).length,"
            " typeof dtf.format(new Date(0)),"
            " pr.select(1),"
            " new Intl.Collator('en-US').compare('a', 'b') < 0,"
            " typeof lf.format(['a', 'b'])"
            "].join('|')"),
        "unit|degree|string|1|string|one|true|string");
}

TEST(JSEngine, DateSupportsCalendarMutationAndStaticConversion) {
    EXPECT_EQ(
        run(
            "const date = new Date('2024-02-29T23:59:58.125Z');"
            "const field = 'Date';"
            "date['set' + field](date['get' + field]() + 1);"
            "date.setUTCMonth(0, 15);"
            "date.setUTCHours(4, 5, 6, 7);"
            "const copy = new Date(date);"
            "["
            " date.toISOString(),"
            " copy.getUTCFullYear(), copy.getUTCMonth(), copy.getUTCDate(),"
            " copy.getUTCHours(), copy.getUTCMinutes(), copy.getUTCSeconds(),"
            " copy.getUTCMilliseconds(),"
            " Date.UTC(99, 0, 1),"
            " Date.parse('1970-01-01T00:00:01.250Z'),"
            " new Date(NaN).toJSON(),"
            " typeof Date()"
            "].join('|')"),
        "2024-01-15T04:05:06.007Z|2024|0|15|4|5|6|7|"
        "915148800000|1250||string");
}

TEST(JSEngine, StringIndexMethodsHonorTheirPosition) {
    EXPECT_EQ(run(
        "const url = '//user@discord.com/path';"
        "[url.indexOf('/', 2), url.lastIndexOf('@'),"
        " 'ababa'.lastIndexOf('ba', 3),"
        " 'ababa'.indexOf('ba', 2)].join('|')"),
        "18|6|3|3");
}

TEST(JSEngine, PerformanceTimelineStoresAndQueriesMarks) {
    EXPECT_EQ(run(
        "performance.clearMarks();"
        "const mark = performance.mark('boot', { startTime: 12.5, detail: { id: 7 } });"
        "performance.mark('ready', { startTime: 20 });"
        "const measure = performance.measure('startup', 'boot', 'ready');"
        "const byName = performance.getEntriesByName('boot', 'mark');"
        "["
        " typeof performance.now(),"
        " typeof performance.timeOrigin,"
        " mark.name, mark.entryType, mark.startTime, mark.duration, mark.detail.id,"
        " byName.length, byName[0] === mark,"
        " measure.entryType, measure.startTime, measure.duration"
        "].join('|')"),
        "number|number|boot|mark|12.5|0|7|1|true|measure|12.5|7.5");
    EXPECT_EQ(run(
        "performance.clearMarks('boot');"
        "performance.clearMeasures();"
        "[performance.getEntriesByType('mark').length,"
        " performance.getEntriesByType('measure').length].join('|')"),
        "0|0");
}

TEST(JSEngine, FunctionPrototypeIsNotEnumerable) {
    EXPECT_EQ(run(
        "function callback() {}"
        "callback.plugin = function() {};"
        "const descriptor = Object.getOwnPropertyDescriptor(callback, 'prototype');"
        "[Object.keys(callback).join(','), descriptor.enumerable,"
        " descriptor.writable, descriptor.configurable].join('|')"),
        "plugin|false|true|false");
}

TEST(JSEngine, FunctionPrototypeIsCallableAndShared) {
    EXPECT_EQ(run(
        "let constructError = '';"
        "try { Reflect.construct(Function.prototype, []); }"
        "catch (error) { constructError = error.name; }"
        "[typeof Function.prototype, String(Function.prototype()),"
        " Object.getPrototypeOf(Function.prototype) === Object.prototype,"
        " Object.getPrototypeOf(function(){}) === Function.prototype,"
        " (function(){}) instanceof Function, constructError].join('|')"),
        "function|undefined|true|true|true|TypeError");
}

TEST(JSEngine, BoundFunctionsForwardConstructionToTheirTarget) {
    EXPECT_EQ(run(
        "function Color(r, g, b) {"
        "  this.values = [r, g, b];"
        "}"
        "Color.prototype.kind = 'color';"
        "const Bound = Function.prototype.bind.apply(Color, [null, 1]);"
        "const value = new Bound(2, 3);"
        "[value.values.join(','), value.kind,"
        " value instanceof Color, value instanceof Bound,"
        " Object.prototype.hasOwnProperty.call(Bound, 'prototype')].join('|')"),
        "1,2,3|color|true|true|false");
    EXPECT_EQ(run(
        "function Factory(value) { return {value: value}; }"
        "(new (Factory.bind(null, 9))()).value"),
        "9");
    EXPECT_EQ(run(
        "let name='';"
        "try { Function.prototype.bind.call({}, null); }"
        "catch (error) { name = error.name; }"
        "name"),
        "TypeError");
}

TEST(JSEngine, UncurriedFunctionToStringSurvivesPrototypePatching) {
    EXPECT_EQ(run(
        "const call = Function.prototype.call;"
        "const uncurryThis = function(method) {"
        "  return function() { return call.apply(method, arguments); };"
        "};"
        "const inspectSource = uncurryThis(Function.toString);"
        "const original = Function.prototype.toString;"
        "Function.prototype.toString = function() {"
        "  return inspectSource(this);"
        "};"
        "function target() {}"
        "const result = target.toString();"
        "Function.prototype.toString = original;"
        "result.indexOf('function target') === 0"),
        "true");
    EXPECT_EQ(run(
        "const call = Function.prototype.call;"
        "const uncurryThis = Function.prototype.bind.bind(call, call);"
        "const inspectSource = uncurryThis(Function.toString);"
        "const original = Function.prototype.toString;"
        "Function.prototype.toString = function() {"
        "  return inspectSource(this);"
        "};"
        "function target() {}"
        "const result = target.toString();"
        "Function.prototype.toString = original;"
        "result.indexOf('function target') === 0"),
        "true");
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

TEST(JSEngine, PendingPromiseCombinatorsKeepCapturedStateAlive) {
    Engine e;
    auto setup = e.evaluate(
        "let resolvePending;"
        "let allValue = '';"
        "let settledValue = '';"
        "const pending = new Promise(resolve => { resolvePending = resolve; });"
        "Promise.all([pending, 2]).then(values => { allValue = values.join('|'); });"
        "Promise.allSettled([pending]).then(values => {"
        "  settledValue = values[0].status + '|' + values[0].value;"
        "});");
    ASSERT_TRUE(setup.ok) << setup.error;

    e.heap().collect();

    auto resolved = e.evaluate("resolvePending(1);");
    ASSERT_TRUE(resolved.ok) << resolved.error;
    auto observed = e.evaluate("allValue + ',' + settledValue");
    ASSERT_TRUE(observed.ok) << observed.error;
    EXPECT_EQ(e.interpreter().to_string(observed.value), u"1|2,fulfilled|1");
}

TEST(JSEngine, MicrotaskCheckpointIsNotReentrant) {
    Engine e;
    auto& in = e.interpreter();
    auto* checkpoint = in.new_native(
        u"checkpoint",
        [](malibu::js::runtime::Interpreter& interpreter,
           malibu::js::runtime::Value,
           std::vector<malibu::js::runtime::Value>&) {
            interpreter.run_microtasks();
            return malibu::js::runtime::Value::make_undefined();
        });
    in.global()->define(
        u"checkpoint",
        malibu::js::runtime::Value::make_heap_ptr(checkpoint));

    auto setup = e.evaluate(
        "let active = false;"
        "let reentered = false;"
        "let runs = 0;"
        "function callback() {"
        "  if (active) reentered = true;"
        "  active = true;"
        "  runs++;"
        "  if (runs === 1) { queueMicrotask(callback); checkpoint(); }"
        "  active = false;"
        "}"
        "queueMicrotask(callback);");
    ASSERT_TRUE(setup.ok) << setup.error;

    auto observed = e.evaluate("[reentered, runs].join('|')");
    ASSERT_TRUE(observed.ok) << observed.error;
    EXPECT_EQ(in.to_string(observed.value), u"false|2");
}

TEST(JSEngine, TextCodecUsesUtf8TypedArrays) {
    EXPECT_EQ(
        run("const bytes = new TextEncoder().encode('A');"
            "const decoded = new TextDecoder().decode("
            "new Uint8Array([195,186]));"
            "[bytes instanceof Uint8Array, bytes.length,"
            "bytes[0], decoded.charCodeAt(0)].join('|')"),
        "true|1|65|250");
}
