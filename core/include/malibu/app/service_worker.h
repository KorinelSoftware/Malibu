#pragma once
// core/include/malibu/app/service_worker.h
// Service Worker host (Task 32 / W3C Service Workers, Requirements 16.3-16.5):
// runs a SW script in its own JS realm, drives the install/activate lifecycle,
// and intercepts fetch events (event.respondWith) backed by the Cache API.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "malibu/js/engine.h"
#include "malibu/storage/cache_storage.h"

namespace malibu::app {

class ServiceWorkerHost {
public:
    explicit ServiceWorkerHost(storage::CacheStorage& caches);

    enum class State { Parsed, Installing, Installed, Activating, Activated, Failed };

    // Evaluates the SW script (which registers its event listeners).
    bool register_script(const std::string& source);

    // Lifecycle: installing → installed (waiting) → activated.
    void install();
    void activate();

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    struct Response {
        int         status = 200;
        std::string body;
    };

    // Routes a fetch through the SW: returns the response if the SW called
    // event.respondWith, otherwise nullopt (caller falls back to the network).
    std::optional<Response> handle_fetch(const std::string& url);

    js::Engine& engine() noexcept { return engine_; }

private:
    void                install_globals();
    js::runtime::Value  make_response(const std::string& body, int status);
    js::runtime::Value  resolved_promise(js::runtime::Value v);
    void                dispatch_event(const std::string& type, js::runtime::Value event);

    js::Engine                                              engine_;
    storage::CacheStorage*                                  caches_;
    State                                                   state_ = State::Parsed;
    std::string                                             error_;
    std::map<std::string, std::vector<js::runtime::Value>>  listeners_;
    std::vector<js::runtime::Value>                         waituntil_;
    js::runtime::Value                                      respond_value_;
    bool                                                    responded_ = false;
};

} // namespace malibu::app
