#pragma once

#include <memory>
#include <string>

#include "malibu/network/fetch_types.h"

namespace malibu::host {

// Stateful host-side HTTP(S) transport. A loader reuses its libcurl connection
// pool and cookie engine across navigations and subresource requests.
class CurlResourceLoader {
public:
    enum class SocketEventType { Open, Message, Close };
    struct SocketEvent {
        SocketEventType type = SocketEventType::Close;
        int id = 0;
        std::string data;
        int code = 1006;
        std::string reason;
    };

    CurlResourceLoader();
    ~CurlResourceLoader();

    CurlResourceLoader(const CurlResourceLoader&) = delete;
    CurlResourceLoader& operator=(const CurlResourceLoader&) = delete;

    bool fetch(const network::FetchRequest& request,
               network::FetchResponse& response);
    bool fetch(const std::string& url, network::FetchResponse& response);
    // `kind`: 0=open, 1=send UTF-8 text, 2=close. Safe to call from the
    // browser thread; network I/O runs on per-connection workers.
    void websocket_command(int id, const std::string& url,
                           const std::string& data, int kind);
    // Drains one worker event. Call from the browser thread and forward it to
    // View::socket_open/message/close.
    bool poll_websocket_event(SocketEvent& event);
    void set_referrer(std::string referrer);
    void set_proxy(std::string proxy);

    [[nodiscard]] const std::string& last_error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace malibu::host
