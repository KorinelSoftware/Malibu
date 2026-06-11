// tests/test_view.cpp
// Task 31: MalibuView end-to-end — load a real HTML+CSS+JS page, render pixels,
// eval JS, bidirectional messaging, request interception, and sandboxing.

#include <gtest/gtest.h>
#include "malibu/view/view.h"

#include <string>

using malibu::view::View;
using malibu::view::SandboxNoNavigation;

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
