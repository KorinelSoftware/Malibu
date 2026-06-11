#pragma once
// malibu_cef.h — a CEF3-compatible embedding subset backed by MalibuView
// (Task 34 / Requirement 18). Provides CefRefPtr ref-counting, the core
// Cef{App,Client,Browser,BrowserHost,Frame} objects, the Cef{LifeSpan,Load,
// Display,Render}Handler callback interfaces, and the global lifecycle
// functions. Unimplemented methods return a zero/null value and log to the
// stub log without crashing.
//
// CefString is std::string here (UTF-8) rather than CEF's UTF-16 CefString —
// a documented simplification of this subset.

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace malibu::view { class View; }

namespace malibu::cef {

using CefString = std::string;

// ---------------------------------------------------------------------------
// Reference counting (CEF's CefBaseRefCounted + CefRefPtr).
// ---------------------------------------------------------------------------
class CefBaseRefCounted {
public:
    virtual ~CefBaseRefCounted() = default;
    void AddRef() const { ref_.fetch_add(1, std::memory_order_relaxed); }
    bool Release() const {
        if (ref_.fetch_sub(1, std::memory_order_acq_rel) == 1) { delete this; return true; }
        return false;
    }
    [[nodiscard]] bool HasOneRef() const { return ref_.load(std::memory_order_acquire) == 1; }
private:
    mutable std::atomic<int> ref_{0};
};

template <class T>
class CefRefPtr {
public:
    CefRefPtr() = default;
    CefRefPtr(std::nullptr_t) {}
    CefRefPtr(T* p) : p_(p) { if (p_) p_->AddRef(); }
    CefRefPtr(const CefRefPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    CefRefPtr(CefRefPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    template <class U> CefRefPtr(const CefRefPtr<U>& o) : p_(o.get()) { if (p_) p_->AddRef(); }
    ~CefRefPtr() { if (p_) p_->Release(); }

    CefRefPtr& operator=(const CefRefPtr& o) { reset(o.p_); return *this; }
    CefRefPtr& operator=(T* p) { reset(p); return *this; }
    CefRefPtr& operator=(CefRefPtr&& o) noexcept {
        if (this != &o) { if (p_) p_->Release(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }

    T* get() const { return p_; }
    T* operator->() const { return p_; }
    T& operator*() const { return *p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void reset(T* p) { if (p) p->AddRef(); if (p_) p_->Release(); p_ = p; }

private:
    T* p_ = nullptr;
};

// Stub log: fully-qualified names of unimplemented CEF methods that were called.
void cef_stub_log(const std::string& method);
const std::vector<std::string>& cef_stub_log_entries();

// ---------------------------------------------------------------------------
// Settings / window info
// ---------------------------------------------------------------------------
struct CefSettings {
    bool        no_sandbox = true;
    bool        multi_threaded_message_loop = false;  // unsupported by MalibuCEF
    std::string log_file;
    // Malibu extension: resolves a URL to document content (the embedder's net).
    std::function<std::optional<std::string>(const std::string& url)> malibu_resource_loader;
};

struct CefWindowInfo {
    int  x = 0, y = 0, width = 800, height = 600;
    bool windowless_rendering_enabled = false;
};

struct CefBrowserSettings {
    int default_font_size = 16;
};

class CefBrowser;
class CefFrame;

// ---------------------------------------------------------------------------
// Handler interfaces (an embedder subclasses these via its CefClient).
// ---------------------------------------------------------------------------
class CefLifeSpanHandler : public CefBaseRefCounted {
public:
    virtual void OnAfterCreated(CefRefPtr<CefBrowser> /*browser*/) {}
    virtual bool DoClose(CefRefPtr<CefBrowser> /*browser*/) { return false; }
    virtual void OnBeforeClose(CefRefPtr<CefBrowser> /*browser*/) {}
};

class CefLoadHandler : public CefBaseRefCounted {
public:
    virtual void OnLoadStart(CefRefPtr<CefBrowser> /*b*/, CefRefPtr<CefFrame> /*f*/) {}
    virtual void OnLoadEnd(CefRefPtr<CefBrowser> /*b*/, CefRefPtr<CefFrame> /*f*/, int /*httpStatusCode*/) {}
    virtual void OnLoadError(CefRefPtr<CefBrowser> /*b*/, CefRefPtr<CefFrame> /*f*/,
                             int /*errorCode*/, const CefString& /*errorText*/, const CefString& /*failedUrl*/) {}
};

class CefDisplayHandler : public CefBaseRefCounted {
public:
    virtual void OnTitleChange(CefRefPtr<CefBrowser> /*b*/, const CefString& /*title*/) {}
    virtual void OnAddressChange(CefRefPtr<CefBrowser> /*b*/, CefRefPtr<CefFrame> /*f*/, const CefString& /*url*/) {}
};

struct CefRect { int x = 0, y = 0, width = 0, height = 0; };

class CefRenderHandler : public CefBaseRefCounted {
public:
    virtual void GetViewRect(CefRefPtr<CefBrowser> /*b*/, CefRect& rect) { rect = CefRect{0, 0, 800, 600}; }
    virtual void OnPaint(CefRefPtr<CefBrowser> /*b*/, const void* /*buffer*/, int /*width*/, int /*height*/) {}
};

class CefClient : public CefBaseRefCounted {
public:
    virtual CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() { return nullptr; }
    virtual CefRefPtr<CefLoadHandler>     GetLoadHandler() { return nullptr; }
    virtual CefRefPtr<CefDisplayHandler>  GetDisplayHandler() { return nullptr; }
    virtual CefRefPtr<CefRenderHandler>   GetRenderHandler() { return nullptr; }
};

class CefApp : public CefBaseRefCounted {
public:
    virtual void OnBeforeCommandLineProcessing(const CefString& /*process_type*/) {}
};

// ---------------------------------------------------------------------------
// Frame / Browser / BrowserHost
// ---------------------------------------------------------------------------
class CefFrame : public CefBaseRefCounted {
public:
    virtual void       LoadURL(const CefString& url) = 0;
    virtual CefString  GetURL() = 0;
    virtual void       ExecuteJavaScript(const CefString& code, const CefString& script_url, int start_line) = 0;
    virtual bool       IsMain() = 0;
    virtual CefString  GetIdentifier() = 0;
    // Malibu extension: the underlying engine view (for embedders/tests).
    virtual malibu::view::View* GetMalibuView() = 0;
};

class CefBrowserHost : public CefBaseRefCounted {
public:
    // CEF3 entry point. Returns null on failure (after OnBeforeClose(null)).
    static CefRefPtr<CefBrowser> CreateBrowserSync(const CefWindowInfo& window_info,
                                                   CefRefPtr<CefClient> client,
                                                   const CefString& url,
                                                   const CefBrowserSettings& settings);

    virtual void                 CloseBrowser(bool force_close) = 0;
    virtual void                 WasResized() = 0;
    virtual CefRefPtr<CefClient> GetClient() = 0;
    // Example unimplemented method (logs to the stub log, returns null).
    virtual void*                GetRequestContext() = 0;
};

class CefBrowser : public CefBaseRefCounted {
public:
    virtual CefRefPtr<CefFrame>       GetMainFrame() = 0;
    virtual CefRefPtr<CefBrowserHost> GetHost() = 0;
    virtual int                       GetIdentifier() = 0;
    virtual bool                      IsLoading() = 0;
};

// ---------------------------------------------------------------------------
// Global lifecycle (CEF3 cef_app.h signatures).
// ---------------------------------------------------------------------------
bool CefInitialize(const CefSettings& settings, CefRefPtr<CefApp> application = nullptr);
void CefShutdown();
void CefRunMessageLoop();
void CefQuitMessageLoop();
bool CefIsInitialized();

} // namespace malibu::cef
