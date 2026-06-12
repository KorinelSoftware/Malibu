// tests/test_view.cpp
// Task 31: MalibuView end-to-end — load a real HTML+CSS+JS page, render pixels,
// eval JS, bidirectional messaging, request interception, and sandboxing.

#include <gtest/gtest.h>
#include "malibu/view/view.h"

#include <string>

using malibu::view::View;
using malibu::view::SandboxNoNavigation;
using malibu::view::LoadDiagnosticKind;

TEST(View, LoadHtmlAndRenderRealPage) {
    View v;
    ASSERT_TRUE(v.load_html(
        "<html><head><style>"
        "#box { background-color:#ff0000; width:40px; height:40px; }"
        "</style></head><body><div id='box'></div></body></html>",
        "https://example.com/"));
    auto fb = v.render(100, 100);
    // body has 8px UA margin → the red box covers ~(8,8)..(48,48).
    EXPECT_EQ(fb.at(20, 20), (malibu::render::Color{255, 0, 0, 255}));
    EXPECT_EQ(fb.at(80, 80), (malibu::render::Color{255, 255, 255, 255}));  // background
}

TEST(View, ScriptMutatesDomAndAffectsRender) {
    View v;
    v.load_html(
        "<html><head><style>"
        "#box { background-color:#ff0000; width:40px; height:40px; }"
        "#box.green { background-color:#00ff00; }"
        "</style></head><body><div id='box'></div>"
        "<script>document.querySelector('#box').setAttribute('class','green');</script>"
        "</body></html>",
        "https://example.com/");
    auto fb = v.render(100, 100);
    EXPECT_EQ(fb.at(20, 20), (malibu::render::Color{0, 255, 0, 255}));  // script turned it green
}

TEST(View, EvalJsReturnsJson) {
    View v;
    v.load_html("<body></body>", "https://example.com/");
    EXPECT_EQ(v.eval_js("1 + 2"), "3");
    EXPECT_EQ(v.eval_js("'a' + 'b'"), "\"ab\"");
    EXPECT_EQ(v.eval_js("({ x: 1, y: [2,3] })"), "{\"x\":1,\"y\":[2,3]}");
}

TEST(View, EvalJsCanReadDom) {
    View v;
    v.load_html("<body><div id='app'>Hi</div></body>", "https://example.com/");
    EXPECT_EQ(v.eval_js("document.querySelector('#app').textContent"), "\"Hi\"");
}

TEST(View, GlobalObjectHasWindowIdentity) {
    View v;
    v.load_html("<body></body>", "https://example.com/");
    EXPECT_EQ(v.eval_js(
        "window === globalThis && self === window && "
        "window instanceof Window && globalThis instanceof Window"),
        "true");
}

TEST(View, DocumentExposesStandardMetadataAsStableValues) {
    View v;
    ASSERT_TRUE(v.load_html(
        "<body></body>", "https://example.com/path/page?x=1"));
    EXPECT_EQ(
        v.eval_js(
            "[document.URL, document.documentURI, document.baseURI,"
            " document.referrer, document.characterSet,"
            " document.contentType, document.compatMode,"
            " document.visibilityState, document.hidden,"
            " document.location === location, document.hasFocus(),"
            " document.activeElement === document.body].join('|')"),
        "\"https://example.com/path/page?x=1|"
        "https://example.com/path/page?x=1|"
        "https://example.com/path/page?x=1||UTF-8|text/html|CSS1Compat|"
        "visible|false|true|true|true\"");
}

TEST(View, JsToNativeMessaging) {
    View v;
    std::string received;
    v.set_message_handler([&](const std::string& m) { received = m; });
    v.load_html("<body></body>", "https://example.com/");
    v.eval_js("malibuNativeMessage('hello from page')");
    EXPECT_EQ(received, "hello from page");
}

TEST(View, NativeToJsMessaging) {
    View v;
    v.load_html(
        "<body><script>"
        "globalThis.last = '';"
        "globalThis.__malibuReceiveMessage = function(m) { globalThis.last = m; };"
        "</script></body>",
        "https://example.com/");
    v.post_message("ping");
    EXPECT_EQ(v.eval_js("globalThis.last"), "\"ping\"");
}

TEST(View, RequestInterception) {
    View v;
    v.set_request_handler([](const std::string& url, malibu::network::FetchResponse& out) {
        if (url == "https://example.com/page") {
            std::string body = "<body><div id='x'>intercepted</div></body>";
            out.body.assign(body.begin(), body.end());
            return true;
        }
        return false;
    });
    ASSERT_TRUE(v.load_url("https://example.com/page"));
    EXPECT_EQ(v.eval_js("document.querySelector('#x').textContent"), "\"intercepted\"");
}

TEST(View, FetchResolvesRelativeUrlsAndReturnsArrayBuffer) {
    View v;
    std::string requested_url;
    v.set_request_handler(
        [&](const std::string& url, malibu::network::FetchResponse& out) {
            requested_url = url;
            out.status = 200;
            out.body = {0x00, 0x61, 0x73, 0x6d};
            return true;
        });
    v.load_html(
        "<script>"
        "fetch('/assets/module.wasm')"
        ".then(response => response.arrayBuffer())"
        ".then(buffer => globalThis.byteLength = buffer.byteLength);"
        "</script>",
        "https://example.com/app/index.html");
    EXPECT_EQ(requested_url, "https://example.com/assets/module.wasm");
    EXPECT_EQ(v.eval_js("globalThis.byteLength"), "4");
}

TEST(View, XMLHttpRequestUsesStructuredRequestsAndDispatchesAsyncEvents) {
    View v;
    malibu::network::FetchRequest observed;
    v.set_fetch_handler(
        [&](const malibu::network::FetchRequest& request,
            malibu::network::FetchResponse& response) {
            observed = request;
            const std::string body = "{\"value\":\"ok\"}";
            response.status = 201;
            response.status_text = "Created";
            response.url = request.url;
            response.headers.set(
                "content-type", "application/json");
            response.headers.set("x-malibu", "yes");
            response.body.assign(body.begin(), body.end());
            response.ok = true;
            response.type =
                malibu::network::ResponseType::Basic;
            return true;
        });
    ASSERT_TRUE(v.load_html(
        "<script>"
        "globalThis.xhrEvents = [];"
        "const xhr = new XMLHttpRequest();"
        "xhr.onreadystatechange = () => "
        "  xhrEvents.push(String(xhr.readyState));"
        "xhr.addEventListener('load', () => "
        "  xhrEvents.push('load:' + xhr.response.value));"
        "xhr.open('POST', '/api/items', true);"
        "xhr.responseType = 'json';"
        "xhr.setRequestHeader('Content-Type', 'text/plain');"
        "xhr.setRequestHeader('X-Test', 'one');"
        "xhr.send('payload');"
        "globalThis.xhr = xhr;"
        "</script>",
        "https://example.com/app/"));

    EXPECT_EQ(observed.method, "POST");
    EXPECT_EQ(observed.url, "https://example.com/api/items");
    EXPECT_EQ(observed.headers.get("content-type"), "text/plain");
    EXPECT_EQ(observed.headers.get("x-test"), "one");
    EXPECT_EQ(
        std::string(
            observed.body.begin(), observed.body.end()),
        "payload");
    EXPECT_EQ(
        v.eval_js(
            "[xhrEvents.join(','), xhr.status, xhr.statusText,"
            " xhr.getResponseHeader('X-Malibu'),"
            " xhr.getAllResponseHeaders().includes("
            "   'content-type: application/json')].join('|')"),
        "\"1,2,3,4,load:ok|201|Created|yes|true\"");
}

TEST(View, WebSocketUsesHostTransportAndDispatchesStandardEvents) {
    View v;
    struct Command {
        int id;
        std::string url;
        std::string data;
        int kind;
    };
    std::vector<Command> commands;
    v.set_socket_handler(
        [&](int id, const std::string& url,
            const std::string& data, int kind) {
            commands.push_back({id, url, data, kind});
        });
    ASSERT_TRUE(v.load_html(
        "<script>"
        "globalThis.socketEvents = [];"
        "globalThis.socket = new WebSocket('/gateway');"
        "socket.onopen = () => socketEvents.push('open');"
        "socket.addEventListener('message', event => "
        "  socketEvents.push('message:' + event.data));"
        "socket.onclose = event => "
        "  socketEvents.push('close:' + event.code + ':' + event.reason);"
        "</script>",
        "https://example.com/app"));
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands[0].url, "wss://example.com/gateway");
    EXPECT_EQ(commands[0].kind, 0);
    const int id = commands[0].id;

    v.socket_open(id);
    EXPECT_EQ(v.eval_js("socket.readyState + '|' + socketEvents.join(',')"),
              "\"1|open\"");
    v.eval_js("socket.send('hello')");
    ASSERT_EQ(commands.size(), 2u);
    EXPECT_EQ(commands[1].data, "hello");
    EXPECT_EQ(commands[1].kind, 1);

    v.socket_message(id, "world");
    EXPECT_EQ(v.eval_js("socketEvents.join(',')"),
              "\"open,message:world\"");
    v.eval_js("socket.close(1000, 'done')");
    ASSERT_EQ(commands.size(), 3u);
    EXPECT_EQ(commands[2].kind, 2);
    v.socket_close(id, 1000, "done");
    EXPECT_EQ(v.eval_js("socket.readyState + '|' + socketEvents.join(',')"),
              "\"3|open,message:world,close:1000:done\"");
}

TEST(View, CacheStoragePersistsResponseObjectsPerOrigin) {
    View v;
    ASSERT_TRUE(v.load_html(
        "<script>"
        "globalThis.cacheResult = '';"
        "caches.open('assets')"
        ".then(cache => cache.put('/cached.txt', "
        "  new Response('cached body', {status: 201})))"
        ".then(() => caches.open('assets'))"
        ".then(cache => cache.match('/cached.txt'))"
        ".then(response => response.text().then(text => {"
        "  globalThis.cacheResult = "
        "    (response instanceof Response) + '|' + "
        "    (response.status) + '|' + text;"
        "}));"
        "</script>",
        "https://example.com/app/"));
    EXPECT_EQ(v.eval_js("globalThis.cacheResult"),
              "\"true|201|cached body\"");
}

TEST(View, WebAssemblyExportsUsableReferenceTables) {
    View v;
    ASSERT_TRUE(v.load_html(
        "<script>"
        "globalThis.tableResult = '';"
        "WebAssembly.instantiate(["
        "0,97,115,109,1,0,0,0,"
        "4,5,1,111,1,2,4,"
        "7,9,1,5,116,97,98,108,101,1,0"
        "], {}).then(result => {"
        "  const table = result.instance.exports.table;"
        "  const oldLength = table.grow(2);"
        "  table.set(1, 'value');"
        "  let rangeError = false;"
        "  try { table.grow(1); } catch (error) {"
        "    rangeError = error instanceof RangeError;"
        "  }"
        "  globalThis.tableResult = ["
        "    oldLength, table.length, table.get(1), table.get(3), rangeError"
        "  ].join('|');"
        "});"
        "</script>",
        "https://example.com/"));
    EXPECT_EQ(v.eval_js("globalThis.tableResult"), "\"2|4|value||true\"");
}

TEST(View, WebAssemblyFunctionTablesExposeCallableReferences) {
    View v;
    ASSERT_TRUE(v.load_html(
        "<script>"
        "globalThis.functionTableResult = '';"
        "WebAssembly.instantiate(["
        "0,97,115,109,1,0,0,0,"
        "1,5,1,96,0,1,127,"
        "3,2,1,0,"
        "4,4,1,112,0,1,"
        "7,9,1,5,116,97,98,108,101,1,0,"
        "9,7,1,0,65,0,11,1,0,"
        "10,6,1,4,0,65,42,11"
        "], {}).then(result => {"
        "  const callback = result.instance.exports.table.get(0);"
        "  globalThis.functionTableResult = "
        "    typeof callback + '|' + callback();"
        "});"
        "</script>",
        "https://example.com/"));
    EXPECT_EQ(v.eval_js("globalThis.functionTableResult"),
              "\"function|42\"");
}

TEST(View, WebAssemblyImportsReturnValuesUsingDeclaredTypes) {
    View v;
    ASSERT_TRUE(v.load_html(
        "<script>"
        "globalThis.importResult = 0;"
        "WebAssembly.instantiate(["
        "0,97,115,109,1,0,0,0,"
        "1,5,1,96,0,1,127,"
        "2,14,1,3,101,110,118,6,97,110,115,119,101,114,0,0,"
        "3,2,1,0,"
        "7,7,1,3,114,117,110,0,1,"
        "10,6,1,4,0,16,0,11"
        "], {env: {answer: function(){ return 42; }}})"
        ".then(result => {"
        "  globalThis.importResult = result.instance.exports.run();"
        "});"
        "</script>",
        "https://example.com/"));
    EXPECT_EQ(v.eval_js("globalThis.importResult"), "42");
}

TEST(View, WebAssemblyExportsMultipleResultsAsAnArray) {
    View v;
    ASSERT_TRUE(v.load_html(
        "<script>"
        "globalThis.multiValueResult = '';"
        "WebAssembly.instantiate(["
        "0,97,115,109,1,0,0,0,"
        "1,7,1,96,0,3,127,127,127,"
        "3,2,1,0,"
        "7,7,1,3,114,117,110,0,0,"
        "10,10,1,8,0,65,4,65,5,65,6,11"
        "], {}).then(result => {"
        "  globalThis.multiValueResult = "
        "    result.instance.exports.run().join('|');"
        "});"
        "</script>",
        "https://example.com/"));
    EXPECT_EQ(v.eval_js("globalThis.multiValueResult"), "\"4|5|6\"");
}

TEST(View, WebAssemblyRoundTripsExternRefValues) {
    View v;
    ASSERT_TRUE(v.load_html(
        "<script>"
        "globalThis.externRefResult = false;"
        "const marker = {value: 7};"
        "WebAssembly.instantiate(["
        "0,97,115,109,1,0,0,0,"
        "1,6,1,96,1,111,1,111,"
        "2,16,1,3,101,110,118,8,105,100,101,110,116,105,116,121,0,0,"
        "3,2,1,0,"
        "7,7,1,3,114,117,110,0,1,"
        "10,8,1,6,0,32,0,16,0,11"
        "], {env: {identity: function(value){ return value; }}})"
        ".then(result => {"
        "  globalThis.externRefResult = "
        "    result.instance.exports.run(marker) === marker;"
        "});"
        "</script>",
        "https://example.com/"));
    EXPECT_EQ(v.eval_js("globalThis.externRefResult"), "true");
}

TEST(View, WebAssemblyHostCallsObserveCurrentLinearMemory) {
    View v;
    ASSERT_TRUE(v.load_html(
        "<script>"
        "globalThis.memorySyncResult = 0;"
        "let wasmMemory = null;"
        "WebAssembly.instantiate(["
        "0,97,115,109,1,0,0,0,"
        "1,10,2,96,1,127,1,127,96,0,1,127,"
        "2,12,1,3,101,110,118,4,114,101,97,100,0,0,"
        "3,2,1,1,"
        "5,3,1,0,1,"
        "7,16,2,6,109,101,109,111,114,121,2,0,"
        "3,114,117,110,0,1,"
        "10,15,1,13,0,65,0,65,42,58,0,0,65,0,16,0,11"
        "], {env: {read: function(offset){"
        "  return new Uint8Array(wasmMemory.buffer)[offset];"
        "}}}).then(result => {"
        "  wasmMemory = result.instance.exports.memory;"
        "  globalThis.memorySyncResult = "
        "    result.instance.exports.run();"
        "});"
        "</script>",
        "https://example.com/"));
    EXPECT_EQ(v.eval_js("globalThis.memorySyncResult"), "42");
}

TEST(View, SandboxBlocksNavigation) {
    View v;
    v.set_sandbox(SandboxNoNavigation);
    v.set_request_handler([](const std::string&, malibu::network::FetchResponse& out) {
        std::string body = "<body></body>"; out.body.assign(body.begin(), body.end()); return true;
    });
    EXPECT_FALSE(v.load_url("https://example.com/"));  // navigation disabled
}

TEST(View, HistoryBackForward) {
    View v;
    v.load_html("<body><div id='p'>one</div></body>", "https://example.com/1");
    v.load_html("<body><div id='p'>two</div></body>", "https://example.com/2");
    EXPECT_EQ(v.current_url(), "https://example.com/2");
    ASSERT_TRUE(v.go_back());
    EXPECT_EQ(v.eval_js("document.querySelector('#p').textContent"), "\"one\"");
    ASSERT_TRUE(v.go_forward());
    EXPECT_EQ(v.eval_js("document.querySelector('#p').textContent"), "\"two\"");
}

TEST(View, RendersRealText) {
    View v;
    v.load_html(
        "<body style='margin:0'><p style='color:#000000; font-size:40px'>Malibu</p></body>",
        "https://example.com/");
    auto fb = v.render(300, 80);
    // Real glyphs (not a solid block) must darken pixels in the text region.
    int dark = 0;
    for (int y = 0; y < fb.height; ++y)
        for (int x = 0; x < fb.width; ++x)
            if (fb.at(x, y).r < 128) ++dark;
    EXPECT_GT(dark, 50);
}

TEST(View, AsyncScriptSettlesViaEventLoop) {
    View v;
    v.load_html(
        "<body><script>"
        "globalThis.done = 0;"
        "async function go(){ let x = await Promise.resolve(21); globalThis.done = x * 2; }"
        "go();"
        "</script></body>",
        "https://example.com/");
    // run_scripts already pumped the event loop at load.
    EXPECT_EQ(v.eval_js("globalThis.done"), "42");
}

TEST(View, DispatchesDOMContentLoadedOnDocument) {
    View v;
    v.load_html(
        "<body><script>"
        "globalThis.documentReady = false;"
        "document.addEventListener('DOMContentLoaded', function(event) {"
        "  globalThis.documentReady = event.target === document;"
        "});"
        "</script></body>",
        "https://example.com/");
    EXPECT_EQ(v.eval_js("globalThis.documentReady"), "true");
}

TEST(View, DataScriptsAreNotEvaluatedAsJavaScript) {
    View v;
    v.load_html(
        "<body><script type='application/ld+json'>{\"name\":\"Malibu\"}</script>"
        "<script>globalThis.afterData = 7;</script></body>",
        "https://example.com/");
    EXPECT_TRUE(v.diagnostics().empty());
    EXPECT_EQ(v.eval_js("globalThis.afterData"), "7");
}

TEST(View, ReportsScriptErrorsAndContinuesLoading) {
    View v;
    v.load_html(
        "<body><script>let = ;</script>"
        "<script>globalThis.afterError = 9;</script></body>",
        "https://example.com/page");
    ASSERT_EQ(v.diagnostics().size(), 1u);
    EXPECT_EQ(v.diagnostics()[0].kind, LoadDiagnosticKind::Script);
    EXPECT_EQ(v.diagnostics()[0].url,
              "https://example.com/page#inline-script-1");
    EXPECT_FALSE(v.diagnostics()[0].message.empty());
    EXPECT_EQ(v.eval_js("globalThis.afterError"), "9");
}

TEST(View, ReportsExternalScriptFetchFailures) {
    View v;
    v.set_request_handler(
        [](const std::string&, malibu::network::FetchResponse&) {
            return false;
        });
    v.load_html("<body><script src='missing.js'></script></body>",
                "https://example.com/path/page");
    ASSERT_EQ(v.diagnostics().size(), 1u);
    EXPECT_EQ(v.diagnostics()[0].kind, LoadDiagnosticKind::Resource);
    EXPECT_EQ(v.diagnostics()[0].url,
              "https://example.com/path/missing.js");
}

TEST(View, LoadsDynamicallyInsertedScriptsAndDispatchesLoad) {
    View v;
    std::vector<std::string> requests;
    v.set_request_handler(
        [&](const std::string& url, malibu::network::FetchResponse& out) {
            requests.push_back(url);
            if (url != "https://example.com/assets/chunk.js") return false;
            const std::string body =
                "globalThis.chunkRan = true;";
            out.status = 200;
            out.body.assign(body.begin(), body.end());
            return true;
        });
    v.load_html(
        "<head></head><body><script>"
        "globalThis.chunkLoaded = false;"
        "const script = document.createElement('script');"
        "script.src = '/assets/chunk.js';"
        "script.onload = function(){ globalThis.chunkLoaded = true; };"
        "document.head.appendChild(script);"
        "</script></body>",
        "https://example.com/app/index.html");

    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0], "https://example.com/assets/chunk.js");
    EXPECT_EQ(v.eval_js("globalThis.chunkRan"), "true");
    EXPECT_EQ(v.eval_js("globalThis.chunkLoaded"), "true");
}

TEST(View, LoadsDynamicallyInsertedStylesheetsAndDispatchesLoad) {
    View v;
    v.set_request_handler(
        [](const std::string& url, malibu::network::FetchResponse& out) {
            if (url != "https://example.com/assets/chunk.css") return false;
            const std::string body =
                "#box { width:20px; height:20px; background:#00ff00; }";
            out.status = 200;
            out.body.assign(body.begin(), body.end());
            return true;
        });
    v.load_html(
        "<head></head><body style='margin:0'><div id='box'></div><script>"
        "globalThis.cssLoaded = false;"
        "const link = document.createElement('link');"
        "link.rel = 'stylesheet';"
        "link.href = '/assets/chunk.css';"
        "link.onload = function(){ globalThis.cssLoaded = true; };"
        "document.head.appendChild(link);"
        "</script></body>",
        "https://example.com/app/index.html");

    EXPECT_EQ(v.eval_js("globalThis.cssLoaded"), "true");
    auto fb = v.render(40, 40);
    EXPECT_EQ(fb.at(10, 10), (malibu::render::Color{0, 255, 0, 255}));
}

TEST(View, ScalesCanvasBackingStoreToCssContentBox) {
    View v;
    ASSERT_TRUE(v.load_html(
        "<body style='margin:0'>"
        "<canvas id='qr' width='2' height='2' "
        "style='display:block;width:20px;height:20px'></canvas>"
        "<script>"
        "const c=document.getElementById('qr');"
        "const x=c.getContext('2d');"
        "x.fillStyle='#ff0000';"
        "x.fillRect(0,0,2,2);"
        "</script></body>",
        "https://example.com/"));

    auto fb = v.render(40, 40);
    EXPECT_EQ(fb.at(15, 15), (malibu::render::Color{255, 0, 0, 255}));
    EXPECT_EQ(fb.at(25, 25), (malibu::render::Color{255, 255, 255, 255}));
}

TEST(View, RasterizesInlineSvgInsertedByScript) {
    View v;
    ASSERT_TRUE(v.load_html(
        "<body style='margin:0'><div id='root'></div><script>"
        "document.getElementById('root').innerHTML="
        "\"<svg style='display:block;width:40px;height:40px' "
        "viewBox='0 0 10 10'><rect width='10' height='10' "
        "fill='#00aa44'></rect></svg>\";"
        "</script></body>",
        "https://example.com/"));

    auto fb = v.render(60, 60);
    EXPECT_EQ(fb.at(20, 20), (malibu::render::Color{0, 170, 68, 255}));
}

TEST(View, IframeExposesAnIsolatedContentDocument) {
    View v;
    v.load_html(
        "<body><iframe id='frame'></iframe><script>"
        "const frame = document.querySelector('#frame');"
        "const nested = frame.contentDocument;"
        "globalThis.hasNestedHead = "
        "nested.getElementsByTagName('head').length === 1;"
        "globalThis.sameDocument = nested === document;"
        "</script></body>",
        "https://example.com/");

    EXPECT_EQ(v.eval_js("globalThis.hasNestedHead"), "true");
    EXPECT_EQ(v.eval_js("globalThis.sameDocument"), "false");
    EXPECT_EQ(v.eval_js(
        "document.querySelector('#frame').contentWindow.document === "
        "document.querySelector('#frame').contentDocument"), "true");
    EXPECT_EQ(v.eval_js(
        "document.querySelector('#frame').contentDocument.head.tagName"),
        "\"HEAD\"");
}

TEST(View, DocumentCookieDefaultsToAStringAndPersistsAssignments) {
    View v;
    v.load_html(
        "<body><script>"
        "globalThis.initialCookieType = typeof document.cookie;"
        "globalThis.initialCookie = document.cookie;"
        "document.cookie = 'session=abc; Path=/; SameSite=Lax';"
        "globalThis.cookieAfterWrite = document.cookie;"
        "</script></body>",
        "https://example.com/app/page");

    EXPECT_EQ(v.eval_js("globalThis.initialCookieType"), "\"string\"");
    EXPECT_EQ(v.eval_js("globalThis.initialCookie"), "\"\"");
    EXPECT_EQ(v.eval_js("globalThis.cookieAfterWrite"), "\"session=abc\"");
    EXPECT_EQ(v.eval_js("document.cookie.indexOf('session=')"), "0");
}

TEST(View, DefersModuleScriptsAndSkipsNoModuleFallbacks) {
    View v;
    v.load_html(
        "<body><script>globalThis.moduleOrder = [];</script>"
        "<script type='module'>"
        "globalThis.moduleOrder.push(globalThis.classicAfterModule);"
        "</script>"
        "<script>globalThis.classicAfterModule = 'ready';</script>"
        "<script nomodule>globalThis.fallbackRan = true;</script></body>",
        "https://example.com/");
    EXPECT_EQ(v.eval_js("globalThis.moduleOrder.join(',')"), "\"ready\"");
    EXPECT_EQ(v.eval_js("typeof globalThis.fallbackRan"), "\"undefined\"");
}

TEST(View, ModuleBindingsAreIsolatedFromClassicScriptClosures) {
    View view;
    view.load_html(
        "<script>"
        "class e { constructor(){ this.source='classic'; } }"
        "class Loader { load(){ return new e().source; } }"
        "window.loader = new Loader();"
        "</script>"
        "<script type='module'>"
        "class e { constructor(){ this.source='module'; } }"
        "window.moduleRan = true;"
        "</script>",
        "https://example.com/");

    EXPECT_EQ(view.eval_js("loader.load()"), "\"classic\"");
    EXPECT_EQ(view.eval_js("moduleRan"), "true");
}

TEST(View, ReportsStylesheetAndImageResourceFailures) {
    View v;
    v.set_request_handler(
        [](const std::string& url, malibu::network::FetchResponse& out) {
            if (url.find("broken.png") != std::string::npos) {
                const std::string invalid = "not an image";
                out.body.assign(invalid.begin(), invalid.end());
                out.status = 200;
                return true;
            }
            return false;
        });
    v.load_html(
        "<link rel='stylesheet' href='missing.css'>"
        "<img src='broken.png'>",
        "https://example.com/path/page");
    ASSERT_EQ(v.diagnostics().size(), 2u);
    EXPECT_EQ(v.diagnostics()[0].kind, LoadDiagnosticKind::Resource);
    EXPECT_EQ(v.diagnostics()[0].url,
              "https://example.com/path/missing.css");
    EXPECT_EQ(v.diagnostics()[1].kind, LoadDiagnosticKind::Resource);
    EXPECT_EQ(v.diagnostics()[1].url,
              "https://example.com/path/broken.png");
}

TEST(View, DedicatedWorkerLoadsAndExchangesStructuredMessages) {
    View v;
    v.set_request_handler(
        [](const std::string& url, malibu::network::FetchResponse& out) {
            if (url != "https://example.com/worker.js") return false;
            const std::string body =
                "postMessage({phase:'ready'});"
                "onmessage=function(event){"
                "  postMessage({phase:'reply',value:event.data.value+1});"
                "};";
            out.status = 200;
            out.url = url;
            out.body.assign(body.begin(), body.end());
            return true;
        });

    ASSERT_TRUE(v.load_html(
        "<script>"
        "globalThis.workerEvents=[];"
        "const worker=new Worker('/worker.js');"
        "worker.onmessage=function(event){"
        "  workerEvents.push(event.data.phase+':'"
        "    +(event.data.value===undefined?'':event.data.value));"
        "};"
        "worker.postMessage({value:41});"
        "</script>",
        "https://example.com/app/index.html"));
    v.run_tasks();

    EXPECT_EQ(v.eval_js("workerEvents.join('|')"),
              "\"ready:|reply:42\"");
}

TEST(View, DomExceptionIsAvailableInsideDedicatedWorkers) {
    View view;
    view.set_request_handler(
        [](const std::string& url,
           malibu::network::FetchResponse& response) {
            if (url != "https://example.test/dom-exception-worker.js")
                return false;
            const std::string source =
                "const error = new DOMException('stopped', 'AbortError');"
                "postMessage(["
                "  error instanceof DOMException,"
                "  error.name, error.message, error.code,"
                "  DOMException.ABORT_ERR, error.toString()"
                "].join('|'));";
            response.status = 200;
            response.body.assign(source.begin(), source.end());
            return true;
        });
    ASSERT_TRUE(view.load_html(
        "<!doctype html><script>"
        "globalThis.workerResult = '';"
        "const worker = new Worker('/dom-exception-worker.js');"
        "worker.onmessage = event => workerResult = event.data;"
        "</script>",
        "https://example.test/"));
    view.run_tasks(10);
    EXPECT_EQ(
        view.eval_js("workerResult"),
        "\"true|AbortError|stopped|20|20|AbortError: stopped\"");
}

TEST(View, AudioCreatesANativeHtmlMediaElementSurface) {
    View v;
    ASSERT_TRUE(v.load_html("<body></body>", "https://example.com/app/"));

    EXPECT_EQ(
        v.eval_js(
            "const audio = new Audio('/sounds/notice.ogg');"
            "[audio.tagName,audio.src,audio.preload,audio.paused,"
            " audio.volume,audio.currentTime,"
            " audio.canPlayType('audio/ogg; codecs=opus'),"
            " audio instanceof HTMLAudioElement,"
            " audio instanceof HTMLMediaElement,"
            " document.createElement('audio') instanceof HTMLAudioElement"
            "].join('|')"),
        "\"AUDIO|/sounds/notice.ogg|auto|true|1|0||true|true|true\"");

    EXPECT_EQ(
        v.eval_js(
            "audio.volume=0.25;audio.currentTime=4;"
            "audio.muted=true;audio.pause();"
            "[audio.volume,audio.currentTime,audio.muted,audio.paused,"
            " audio.buffered.length].join('|')"),
        "\"0.25|4|true|true|0\"");
}

TEST(View, ImageCreatesANativeHtmlImageElementSurface) {
    View view;
    ASSERT_TRUE(view.load_html(
        "<!doctype html><div id='root'></div>",
        "https://example.test/"));
    EXPECT_EQ(
        view.eval_js(
            "const image = new Image(320, 180);"
            "image.src = '/poster.png';"
            "document.getElementById('root').appendChild(image);"
            "["
            " image.tagName, image.width, image.height,"
            " image.getAttribute('src'),"
            " image instanceof HTMLImageElement"
            "].join('|')"),
        "\"IMG|320|180|/poster.png|true\"");
}

TEST(View, CancelAnimationFramePreventsTheCallback) {
    View view;
    ASSERT_TRUE(view.load_html(
        "<!doctype html><div></div>",
        "https://example.test/"));
    EXPECT_EQ(
        view.eval_js(
            "let calls = 0;"
            "const id = requestAnimationFrame(() => calls++);"
            "cancelAnimationFrame(id);"
            "calls"),
        "0");
    view.run_tasks(20);
    EXPECT_EQ(view.eval_js("calls"), "0");
}

TEST(View, PerformanceExposesNavigationTimingEntry) {
    View view;
    ASSERT_TRUE(view.load_html(
        "<!doctype html><div></div>",
        "https://example.test/path"));
    EXPECT_EQ(
        view.eval_js(
            "const navigation=performance.getEntriesByType('navigation')[0];"
            "[typeof performance.now,performance.now()>=0,"
            " performance.timeOrigin>0,navigation.entryType,"
            " navigation.responseStart>0,"
            " performance.timing.responseStart>0].join('|')"),
        "\"function|true|true|navigation|true|true\"");
}

TEST(View, ElementAnimateReturnsAUsableAnimation) {
    View view;
    ASSERT_TRUE(view.load_html(
        "<!doctype html><div id='box'></div>",
        "https://example.test/"));
    EXPECT_EQ(
        view.eval_js(
            "let finished=false;"
            "const animation=document.getElementById('box').animate("
            " [{opacity:0},{opacity:1}],{duration:100});"
            "animation.finished.then(()=>finished=true);"
            "animation.pause();"
            "animation.updatePlaybackRate(2);"
            "[animation.playState,animation.playbackRate,"
            " animation.effect.target.id,"
            " typeof Element.prototype.animate,"
            " document.timeline.currentTime].join('|')"),
        "\"paused|2|box|function|0\"");
    view.run_tasks();
    EXPECT_EQ(view.eval_js("finished"), "true");
}

TEST(View, FormCollectionsAndRequestSubmitNavigateWithSuccessfulControls) {
    View view;
    std::vector<std::string> requests;
    view.set_request_handler(
        [&](const std::string& url, malibu::network::FetchResponse& response) {
            requests.push_back(url);
            const std::string body =
                "<!doctype html><div id='destination'>arrived</div>";
            response.url = url;
            response.status = 200;
            response.body.assign(body.begin(), body.end());
            return true;
        });
    ASSERT_TRUE(view.load_html(
        "<!doctype html><form id='challenge' name='challenge' action='/done'>"
        "<input name='solution' value='old'>"
        "<input name='skip' disabled value='no'>"
        "</form><script>"
        "const form=document.forms[0];"
        "form.onsubmit=function(event){"
        " event.target.appendChild(Object.assign("
        "  document.createElement('input'),"
        "  {name:'token',type:'hidden',value:'a b&c'}));"
        " form.elements.namedItem('solution').value='solved';"
        " return true;"
        "};"
        "form.requestSubmit();"
        "</script>",
        "https://example.test/challenge?old=query"));
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(
        requests[0],
        "https://example.test/done?solution=solved&token=a+b%26c");
    EXPECT_EQ(view.current_url(), requests[0]);
    EXPECT_EQ(
        view.eval_js(
            "[document.forms.length,"
            " document.getElementById('destination').textContent].join('|')"),
        "\"0|arrived\"");
}

TEST(View, CustomElementsUpgradeExistingAndDynamicallyConnectedElements) {
    View view;
    ASSERT_TRUE(view.load_html(
        "<!doctype html><body><x-panel id='existing'></x-panel></body>",
        "https://example.test/"));
    EXPECT_EQ(
        view.eval_js(
            "class XPanel extends HTMLElement {"
            " constructor(){super();this.constructed=4;}"
            " connectedCallback(){this.connected=(this.constructed||0)+1;}"
            " ping(){return this.connected;}"
            "}"
            "const pending=customElements.whenDefined('x-panel');"
            "customElements.define('x-panel',XPanel);"
            "const dynamic=document.createElement('x-panel');"
            "document.body.appendChild(dynamic);"
            "let resolved=false;pending.then(()=>resolved=true);"
            "[customElements.get('x-panel')===XPanel,"
            " customElements.getName(XPanel),"
            " document.getElementById('existing').ping(),"
            " dynamic.ping(),dynamic.isConnected].join('|')"),
        "\"true|x-panel|5|5|true\"");
    view.run_tasks();
    EXPECT_EQ(view.eval_js("resolved"), "true");
}

TEST(View, AttachShadowReturnsAReusableDocumentFragment) {
    View view;
    ASSERT_TRUE(view.load_html(
        "<!doctype html><div id='host'></div>",
        "https://example.test/"));
    EXPECT_EQ(
        view.eval_js(
            "const host=document.getElementById('host');"
            "const root=host.attachShadow({mode:'open',delegatesFocus:true});"
            "root.appendChild(document.createElement('span'));"
            "[host.shadowRoot===root,root.host===host,root.mode,"
            " root.delegatesFocus,root.childNodes.length,"
            " host.attachShadow({mode:'open'})===root].join('|')"),
        "\"true|true|open|true|1|true\"");
}

TEST(View, ModuleScriptsExecuteInlineAndFromDataUrls) {
    View view;
    ASSERT_TRUE(view.load_html(
        "<!doctype html><body>"
        "<script type='module'>window.moduleTotal=1;export default 7;</script>"
        "<script type='module' "
        "src='data:text/javascript,window.moduleTotal%2B%3D2%3B"
        "var%20answer%3D42%3Bexport%20%7Banswer%20as%20default%7D%3B'>"
        "</script>"
        "<script nomodule>window.moduleTotal=99;</script>"
        "</body>",
        "https://example.test/"));
    EXPECT_EQ(view.eval_js("window.moduleTotal"), "3");
    for (const auto& diagnostic : view.diagnostics())
        EXPECT_EQ(
            diagnostic.message.find(
                "module scripts are not implemented"),
            std::string::npos);
}

TEST(View, ExposesModernScriptElementAndDOMParserInterfaces) {
    View view;
    ASSERT_TRUE(view.load_html(
        "<!doctype html><body><script id='source'></script></body>",
        "https://example.test/"));
    EXPECT_EQ(
        view.eval_js(
            "const parsed=new DOMParser().parseFromString("
            "'<!doctype html><html><body><iframe src=\"/inside\"></iframe>'"
            ",'text/html');"
            "[typeof HTMLScriptElement,"
            " 'noModule' in HTMLScriptElement.prototype,"
            " document.getElementById('source').constructor===HTMLScriptElement,"
            " parsed.querySelector('iframe').getAttribute('src')].join('|')"),
        "\"function|true|true|/inside\"");
}

TEST(View, DynamicImportLoadsAndEvaluatesDataModules) {
    View view;
    ASSERT_TRUE(view.load_html(
        "<!doctype html><body><script>"
        "window.dynamicTotal=1;"
        "window.dynamicDone=false;"
        "import('data:text/javascript,window.dynamicTotal%2B%3D4%3B"
        "export%20default%205%3B').then(()=>window.dynamicDone=true);"
        "</script></body>",
        "https://example.test/"));
    view.run_tasks();
    EXPECT_EQ(view.eval_js("window.dynamicTotal"), "5");
    EXPECT_EQ(view.eval_js("window.dynamicDone"), "true");
}

TEST(View, ImportMetaExposesTheEvaluatedModuleURL) {
    View view;
    ASSERT_TRUE(view.load_html(
        "<!doctype html><body>"
        "<script type='module' "
        "src='data:text/javascript,"
        "window.moduleURL%3Dimport.meta.url%3B'></script>"
        "</body>",
        "https://example.test/app/"));
    EXPECT_EQ(
        view.eval_js("window.moduleURL"),
        "\"data:text/javascript,window.moduleURL%3Dimport.meta.url%3B\"");
}

TEST(View, AbortControllerOwnsAnObservableAbortSignal) {
    View view;
    ASSERT_TRUE(view.load_html(
        "<!doctype html><div></div>",
        "https://example.test/"));
    EXPECT_EQ(
        view.eval_js(
            "const controller = new AbortController();"
            "let events = 0;"
            "controller.signal.addEventListener('abort', () => events++);"
            "controller.signal.onabort = () => events++;"
            "controller.abort('stop');"
            "controller.abort('ignored');"
            "let thrown = '';"
            "try { controller.signal.throwIfAborted(); }"
            "catch (error) { thrown = error; }"
            "[controller instanceof AbortController,"
            " controller.signal instanceof AbortSignal,"
            " controller.signal.aborted, controller.signal.reason,"
            " events, thrown].join('|')"),
        "\"true|true|true|stop|2|stop\"");
}

TEST(View, AbortSignalTimeoutDispatchesThroughTheEventLoop) {
    View view;
    ASSERT_TRUE(view.load_html(
        "<!doctype html><script>"
        "globalThis.timeoutSignal = AbortSignal.timeout(5);"
        "globalThis.timeoutEvents = 0;"
        "timeoutSignal.addEventListener("
        "  'abort', () => timeoutEvents++);"
        "</script>",
        "https://example.test/"));
    EXPECT_EQ(
        view.eval_js(
            "[timeoutSignal.aborted, timeoutEvents].join('|')"),
        "\"false|0\"");
    view.run_tasks(5);
    EXPECT_EQ(
        view.eval_js(
            "[timeoutSignal.aborted, timeoutSignal.reason.name,"
            " timeoutEvents].join('|')"),
        "\"true|TimeoutError|1\"");
}

TEST(View, FetchRejectsAnAlreadyAbortedSignalWithoutNetworkIo) {
    View view;
    int requests = 0;
    view.set_request_handler(
        [&](const std::string&,
            malibu::network::FetchResponse&) {
            ++requests;
            return true;
        });
    ASSERT_TRUE(view.load_html(
        "<!doctype html><script>"
        "const controller = new AbortController();"
        "controller.abort('cancelled');"
        "globalThis.fetchReason = '';"
        "fetch('/never', {signal: controller.signal})"
        "  .catch(reason => fetchReason = reason);"
        "</script>",
        "https://example.test/"));
    EXPECT_EQ(requests, 0);
    EXPECT_EQ(view.eval_js("fetchReason"), "\"cancelled\"");
}

TEST(View, NodesExposeTheirOwnerDocumentForReactEventDelegation) {
    View v;
    ASSERT_TRUE(v.load_html(
        "<body><div id='root'></div></body>",
        "https://example.com/app/"));

    EXPECT_EQ(
        v.eval_js(
            "const root=document.getElementById('root');"
            "const key='_reactListening';"
            "root[key]=true;"
            "const eventDocument=9===root.nodeType?root:root.ownerDocument;"
            "eventDocument[key]=true;"
            "[root.ownerDocument===document,"
            " document.ownerDocument===null,"
            " document.defaultView===window,"
            " root[key],document[key]].join('|')"),
        "\"true|true|true|true|true\"");
}

TEST(View, DomNodesExposeTheirConcreteWebIdlConstructor) {
    View view;
    ASSERT_TRUE(view.load_html(
        "<body><input id='field'><div id='box'></div></body>",
        "https://example.test/"));
    EXPECT_EQ(
        view.eval_js(
            "const field = document.getElementById('field');"
            "const descriptor = Object.getOwnPropertyDescriptor("
            "  field.constructor.prototype, 'value');"
            "["
            " field.constructor === HTMLInputElement,"
            " field instanceof HTMLInputElement,"
            " descriptor === undefined,"
            " field.hasOwnProperty('value') === false,"
            " typeof field.toString === 'function',"
            " document.getElementById('box').constructor === "
            "   HTMLDivElement,"
            " document.constructor === Document"
            "].join('|')"),
        "\"true|true|true|true|true|true|true\"");
}

TEST(View, HtmlElementFocusUpdatesTheActiveElementAndDispatchesEvents) {
    View view;
    ASSERT_TRUE(view.load_html(
        "<body><input id='field'></body>",
        "https://example.test/"));
    EXPECT_EQ(
        view.eval_js(
            "const field = document.getElementById('field');"
            "const events = [];"
            "field.addEventListener('focus', () => events.push('focus'));"
            "field.addEventListener('blur', () => events.push('blur'));"
            "field.focus();"
            "const focused = document.activeElement === field;"
            "field.blur();"
            "[focused, document.activeElement === document.body,"
            " events.join(',')].join('|')"),
        "\"true|true|focus,blur\"");
}

TEST(View, CharacterDataSettersMutateTextAndRefreshGeometry) {
    View view;
    ASSERT_TRUE(view.load_html(
        "<body><span id='label' style='display:inline-block'>x</span></body>",
        "https://example.test/"));
    (void)view.render(800, 600);

    EXPECT_EQ(
        view.eval_js(
            "const label=document.getElementById('label');"
            "const text=label.firstChild;"
            "const before=label.getBoundingClientRect().width;"
            "text.nodeValue='a much longer label';"
            "const after=label.getBoundingClientRect().width;"
            "const viaNodeValue=text.nodeValue;"
            "text.data='updated';"
            "const viaData=label.textContent;"
            "text.nodeValue=null;"
            "[viaNodeValue,viaData,label.textContent,after>before].join('|')"),
        "\"a much longer label|updated||true\"");
}

TEST(View, CssNamespaceExposesCapabilityQueriesAndEscaping) {
    View v;
    ASSERT_TRUE(v.load_html("<body></body>", "https://example.com/app/"));

    EXPECT_EQ(
        v.eval_js(
            "[typeof CSS,typeof CSS.supports,"
            " CSS.supports('display','grid'),"
            " CSS.supports('(position: sticky)'),"
            " CSS.supports('selector(::-webkit-scrollbar)'),"
            " CSS.escape('menu item#1'),"
            " window.CSS===CSS].join('|')"),
        "\"object|function|true|true|false|menu\\\\ item\\\\#1|true\"");
}
