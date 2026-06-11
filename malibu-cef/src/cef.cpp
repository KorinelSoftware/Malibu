// malibu-cef/src/cef.cpp
// MalibuCEF: CEF3-compatible embedding subset backed by malibu::view::View.

#include "malibu_cef.h"
#include "malibu/view/view.h"
#include "malibu/diagnostics/diagnostic_log.h"

namespace malibu::cef {
namespace {
bool                                                              g_initialized = false;
bool                                                              g_quit = false;
std::function<std::optional<std::string>(const std::string&)>     g_resource_loader;
std::vector<std::string>                                          g_stub_log;
int                                                               g_next_id = 1;
}  // namespace

void cef_stub_log(const std::string& method) {
    g_stub_log.push_back(method);
    MALIBU_LOG(WARNING, "cef", "unimplemented CEF method: " + method);
}
const std::vector<std::string>& cef_stub_log_entries() { return g_stub_log; }

// ---------------------------------------------------------------------------
// Internal implementations
// ---------------------------------------------------------------------------
namespace {

class CefBrowserImpl;

class CefFrameImpl : public CefFrame {
public:
    CefFrameImpl(CefBrowserImpl* browser, view::View* view) : browser_(browser), view_(view) {}

    void      LoadURL(const CefString& url) override;
    CefString GetURL() override { return url_; }
    void      ExecuteJavaScript(const CefString& code, const CefString&, int) override { view_->eval_js(code); }
    bool      IsMain() override { return true; }
    CefString GetIdentifier() override { return "main"; }
    view::View* GetMalibuView() override { return view_; }

private:
    CefBrowserImpl* browser_;
    view::View*     view_;
    CefString       url_;
};

class CefBrowserHostImpl : public CefBrowserHost {
public:
    explicit CefBrowserHostImpl(CefBrowserImpl* b) : browser_(b) {}
    void                 CloseBrowser(bool) override;
    void                 WasResized() override {}
    CefRefPtr<CefClient> GetClient() override;
    void*                GetRequestContext() override {
        cef_stub_log("CefBrowserHost::GetRequestContext");
        return nullptr;
    }
private:
    CefBrowserImpl* browser_;
};

class CefBrowserImpl : public CefBrowser {
public:
    explicit CefBrowserImpl(CefRefPtr<CefClient> client) : client_(client), id_(g_next_id++) {
        view_ = std::make_unique<view::View>();
        if (g_resource_loader) {
            auto loader = g_resource_loader;
            view_->set_request_handler([loader](const std::string& url, network::FetchResponse& out) {
                auto content = loader(url);
                if (!content) return false;
                out.body.assign(content->begin(), content->end());
                out.status = 200;
                return true;
            });
        }
        frame_ = new CefFrameImpl(this, view_.get());
        host_ = new CefBrowserHostImpl(this);
    }

    CefRefPtr<CefFrame>       GetMainFrame() override { return frame_.get(); }
    CefRefPtr<CefBrowserHost> GetHost() override { return host_.get(); }
    int                       GetIdentifier() override { return id_; }
    bool                      IsLoading() override { return loading_; }

    CefClient*  client() { return client_.get(); }
    void        set_loading(bool v) { loading_ = v; }
    view::View* view() { return view_.get(); }

private:
    CefRefPtr<CefClient>            client_;
    std::unique_ptr<view::View>     view_;
    CefRefPtr<CefFrameImpl>         frame_;
    CefRefPtr<CefBrowserHostImpl>   host_;
    int                             id_;
    bool                            loading_ = false;
};

void CefFrameImpl::LoadURL(const CefString& url) {
    url_ = url;
    CefRefPtr<CefBrowser> b(browser_);
    CefRefPtr<CefFrame> f(this);
    CefClient* client = browser_->client();

    browser_->set_loading(true);
    if (client) if (auto lh = client->GetLoadHandler()) lh->OnLoadStart(b, f);
    bool ok = view_->load_url(url);
    browser_->set_loading(false);
    if (client) {
        if (auto dh = client->GetDisplayHandler()) dh->OnAddressChange(b, f, url);
        if (auto lh = client->GetLoadHandler()) lh->OnLoadEnd(b, f, ok ? 200 : 404);
    }
}

void CefBrowserHostImpl::CloseBrowser(bool) {
    CefRefPtr<CefBrowser> b(browser_);
    if (CefClient* client = browser_->client())
        if (auto ls = client->GetLifeSpanHandler()) ls->OnBeforeClose(b);
}
CefRefPtr<CefClient> CefBrowserHostImpl::GetClient() { return browser_->client(); }

}  // namespace

// ---------------------------------------------------------------------------
// CefBrowserHost::CreateBrowserSync
// ---------------------------------------------------------------------------
CefRefPtr<CefBrowser> CefBrowserHost::CreateBrowserSync(const CefWindowInfo&,
                                                       CefRefPtr<CefClient> client,
                                                       const CefString& url,
                                                       const CefBrowserSettings&) {
    if (!g_initialized) {
        MALIBU_LOG(ERROR, "cef", "CreateBrowserSync before CefInitialize");
        if (client) if (auto ls = client->GetLifeSpanHandler()) ls->OnBeforeClose(nullptr);
        return nullptr;
    }
    auto* browser = new CefBrowserImpl(client);
    CefRefPtr<CefBrowser> ref(browser);
    if (client) if (auto ls = client->GetLifeSpanHandler()) ls->OnAfterCreated(ref);
    if (!url.empty()) browser->GetMainFrame()->LoadURL(url);
    return ref;
}

// ---------------------------------------------------------------------------
// Global lifecycle
// ---------------------------------------------------------------------------
bool CefInitialize(const CefSettings& settings, CefRefPtr<CefApp> /*application*/) {
    if (g_initialized) return true;
    bool ok = true;
    if (settings.multi_threaded_message_loop) {
        MALIBU_LOG(WARNING, "cef",
                   "unsupported setting: multi_threaded_message_loop (ignored)");
        ok = false;  // partial initialization (Req 18.6)
    }
    g_resource_loader = settings.malibu_resource_loader;
    g_quit = false;
    g_initialized = true;
    return ok;
}

void CefShutdown() { g_initialized = false; g_resource_loader = nullptr; }

void CefRunMessageLoop() {
    // A real backend blocks on the platform message pump; this subset returns
    // once a quit has been requested (no platform event source is wired).
    g_quit = false;
    while (!g_quit) break;
}

void CefQuitMessageLoop() { g_quit = true; }
bool CefIsInitialized() { return g_initialized; }

} // namespace malibu::cef
