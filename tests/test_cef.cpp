// tests/test_cef.cpp
// Task 34: MalibuCEF — CEF3-compatible embedding backed by MalibuView.

#include <gtest/gtest.h>
#include "malibu_cef.h"
#include "malibu/view/view.h"

#include <optional>
#include <string>

using namespace malibu::cef;

namespace {
struct TestClient : CefClient {
    struct Life : CefLifeSpanHandler {
        bool after_created = false;
        bool before_close = false;
        bool before_close_null = false;
        void OnAfterCreated(CefRefPtr<CefBrowser> b) override { after_created = (b.get() != nullptr); }
        void OnBeforeClose(CefRefPtr<CefBrowser> b) override {
            before_close = true;
            before_close_null = (b.get() == nullptr);
        }
    };
    struct Load : CefLoadHandler {
        int  end_status = -1;
        bool started = false;
        void OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>) override { started = true; }
        void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int status) override { end_status = status; }
    };
    CefRefPtr<Life> life{new Life()};
    CefRefPtr<Load> load{new Load()};
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return life.get(); }
    CefRefPtr<CefLoadHandler>     GetLoadHandler() override { return load.get(); }
};

CefSettings settings_with_loader() {
    CefSettings s;
    s.malibu_resource_loader = [](const std::string& url) -> std::optional<std::string> {
        if (url == "https://app.local/index.html")
            return std::string("<body><h1 id='t'>CEF on Malibu</h1></body>");
        return std::nullopt;
    };
    return s;
}
}  // namespace

TEST(Cef, InitializeAndShutdown) {
    EXPECT_TRUE(CefInitialize(settings_with_loader()));
    EXPECT_TRUE(CefIsInitialized());
    CefShutdown();
    EXPECT_FALSE(CefIsInitialized());
}

TEST(Cef, UnsupportedSettingReturnsPartialInit) {
    CefSettings s = settings_with_loader();
    s.multi_threaded_message_loop = true;  // unsupported
    EXPECT_FALSE(CefInitialize(s));         // partial init (Req 18.6)
    CefShutdown();
}

TEST(Cef, CreateBrowserLoadsAndFiresHandlers) {
    ASSERT_TRUE(CefInitialize(settings_with_loader()));
    CefRefPtr<TestClient> client(new TestClient());
    CefWindowInfo info;
    CefBrowserSettings bs;
    CefRefPtr<CefBrowser> browser =
        CefBrowserHost::CreateBrowserSync(info, client.get(), "https://app.local/index.html", bs);
    ASSERT_TRUE(browser);
    EXPECT_TRUE(client->life->after_created);
    EXPECT_TRUE(client->load->started);
    EXPECT_EQ(client->load->end_status, 200);

    // The page loaded into a real MalibuView; query its DOM via JS.
    CefRefPtr<CefFrame> frame = browser->GetMainFrame();
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->GetURL(), "https://app.local/index.html");
    EXPECT_EQ(frame->GetMalibuView()->eval_js("document.querySelector('#t').textContent"),
              "\"CEF on Malibu\"");

    browser->GetHost()->CloseBrowser(true);
    EXPECT_TRUE(client->life->before_close);
    CefShutdown();
}

TEST(Cef, CreateBrowserBeforeInitFailsGracefully) {
    CefShutdown();  // ensure uninitialized
    CefRefPtr<TestClient> client(new TestClient());
    CefWindowInfo info;
    CefBrowserSettings bs;
    CefRefPtr<CefBrowser> browser =
        CefBrowserHost::CreateBrowserSync(info, client.get(), "https://app.local/index.html", bs);
    EXPECT_FALSE(browser);                          // null on failure (Req 18.5)
    EXPECT_TRUE(client->life->before_close);
    EXPECT_TRUE(client->life->before_close_null);    // OnBeforeClose(null)
}

TEST(Cef, UnimplementedMethodLogged) {
    ASSERT_TRUE(CefInitialize(settings_with_loader()));
    CefRefPtr<TestClient> client(new TestClient());
    CefWindowInfo info; CefBrowserSettings bs;
    auto browser = CefBrowserHost::CreateBrowserSync(info, client.get(), "", bs);
    ASSERT_TRUE(browser);
    size_t before = cef_stub_log_entries().size();
    EXPECT_EQ(browser->GetHost()->GetRequestContext(), nullptr);  // unimplemented → null + log
    ASSERT_EQ(cef_stub_log_entries().size(), before + 1);
    EXPECT_NE(cef_stub_log_entries().back().find("GetRequestContext"), std::string::npos);
    CefShutdown();
}

TEST(Cef, ExecuteJavaScriptRunsInView) {
    ASSERT_TRUE(CefInitialize(settings_with_loader()));
    CefRefPtr<TestClient> client(new TestClient());
    CefWindowInfo info; CefBrowserSettings bs;
    auto browser = CefBrowserHost::CreateBrowserSync(info, client.get(), "https://app.local/index.html", bs);
    ASSERT_TRUE(browser);
    auto frame = browser->GetMainFrame();
    frame->ExecuteJavaScript("globalThis.x = 7 * 6;", "", 0);
    EXPECT_EQ(frame->GetMalibuView()->eval_js("globalThis.x"), "42");
    CefShutdown();
}
