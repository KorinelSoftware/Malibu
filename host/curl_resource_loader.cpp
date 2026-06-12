#include "malibu/host/curl_resource_loader.h"

#include <curl/curl.h>
#include <curl/websockets.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace malibu::host {
namespace {

void initialize_curl() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t write_body(void* contents, size_t size, size_t count, void* user_data) {
    auto* body = static_cast<std::vector<uint8_t>*>(user_data);
    const size_t bytes = size * count;
    const auto* first = static_cast<const uint8_t*>(contents);
    body->insert(body->end(), first, first + bytes);
    return bytes;
}

size_t write_header(void* contents, size_t size, size_t count,
                    void* user_data) {
    auto* headers = static_cast<network::HeaderMap*>(user_data);
    const size_t bytes = size * count;
    std::string line(static_cast<const char*>(contents), bytes);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    const size_t colon = line.find(':');
    if (colon == std::string::npos) return bytes;
    std::string value = line.substr(colon + 1);
    const size_t first = value.find_first_not_of(" \t");
    value = first == std::string::npos ? std::string() : value.substr(first);
    headers->set(line.substr(0, colon), value);
    return bytes;
}

} // namespace

struct CurlResourceLoader::Impl {
    struct SocketCommand {
        std::string data;
        bool close = false;
        int code = 1000;
        std::string reason;
    };

    struct SocketConnection {
        int id = 0;
        std::string url;
        std::mutex mutex;
        std::deque<SocketCommand> commands;
        bool stopping = false;
        std::thread worker;
    };

    CURL* handle = nullptr;
    std::string referrer;
    std::string proxy;
    std::string error;
    char error_buffer[CURL_ERROR_SIZE] = {};
    std::mutex sockets_mutex;
    std::map<int, std::shared_ptr<SocketConnection>> sockets;
    std::mutex events_mutex;
    std::deque<CurlResourceLoader::SocketEvent> socket_events;

    Impl() {
        initialize_curl();
        handle = curl_easy_init();
        if (!handle) {
            error = "curl_easy_init failed";
            return;
        }
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle, CURLOPT_AUTOREFERER, 1L);
        curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 20L);
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(handle, CURLOPT_HTTP_VERSION,
                         CURL_HTTP_VERSION_2TLS);
        curl_easy_setopt(
            handle, CURLOPT_USERAGENT,
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Malibu/0.1 Safari/537.36");
        curl_easy_setopt(handle, CURLOPT_COOKIEFILE, "");
        if (const char* cookie_jar = std::getenv("MALIBU_COOKIE_JAR")) {
            curl_easy_setopt(handle, CURLOPT_COOKIEFILE, cookie_jar);
            curl_easy_setopt(handle, CURLOPT_COOKIEJAR, cookie_jar);
        }
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_body);
        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, write_header);
        curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_buffer);
    }

    ~Impl() {
        std::vector<std::shared_ptr<SocketConnection>> connections;
        {
            std::lock_guard lock(sockets_mutex);
            for (auto& [id, connection] : sockets) {
                (void)id;
                {
                    std::lock_guard connection_lock(connection->mutex);
                    connection->stopping = true;
                }
                connections.push_back(connection);
            }
            sockets.clear();
        }
        for (auto& connection : connections)
            if (connection->worker.joinable()) connection->worker.join();
        if (handle) curl_easy_cleanup(handle);
    }

    void push_socket_event(CurlResourceLoader::SocketEvent event) {
        std::lock_guard lock(events_mutex);
        socket_events.push_back(std::move(event));
    }

    static std::pair<int, std::string> decode_close(
        const std::string& payload) {
        const size_t newline = payload.find('\n');
        if (newline == std::string::npos) return {1000, payload};
        int code = 1000;
        try {
            code = std::stoi(payload.substr(0, newline));
        } catch (...) {
        }
        return {code, payload.substr(newline + 1)};
    }

    void run_websocket(
        const std::shared_ptr<SocketConnection>& connection) {
        CURL* socket = curl_easy_init();
        if (!socket) {
            push_socket_event(
                {CurlResourceLoader::SocketEventType::Close,
                 connection->id, "", 1006, "curl_easy_init failed"});
            return;
        }
        char socket_error[CURL_ERROR_SIZE] = {};
        curl_easy_setopt(socket, CURLOPT_URL, connection->url.c_str());
        curl_easy_setopt(socket, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(socket, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(socket, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(socket, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(socket, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(socket, CURLOPT_ERRORBUFFER, socket_error);
        curl_easy_setopt(
            socket, CURLOPT_USERAGENT,
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Malibu/0.1 Safari/537.36");
        if (!proxy.empty())
            curl_easy_setopt(socket, CURLOPT_PROXY, proxy.c_str());
        if (!referrer.empty())
            curl_easy_setopt(socket, CURLOPT_REFERER, referrer.c_str());
        curl_easy_setopt(socket, CURLOPT_COOKIEFILE, "");
        if (const char* cookie_jar = std::getenv("MALIBU_COOKIE_JAR")) {
            curl_easy_setopt(socket, CURLOPT_COOKIEFILE, cookie_jar);
            curl_easy_setopt(socket, CURLOPT_COOKIEJAR, cookie_jar);
        }

        CURLcode result = curl_easy_perform(socket);
        if (result != CURLE_OK) {
            std::string message =
                socket_error[0] != '\0'
                    ? socket_error
                    : curl_easy_strerror(result);
            curl_easy_cleanup(socket);
            push_socket_event(
                {CurlResourceLoader::SocketEventType::Close,
                 connection->id, "", 1006, std::move(message)});
            return;
        }
        push_socket_event(
            {CurlResourceLoader::SocketEventType::Open,
             connection->id, "", 0, ""});

        std::vector<uint8_t> incoming;
        std::vector<uint8_t> receive_buffer(64 * 1024);
        bool finished = false;
        int close_code = 1000;
        std::string close_reason;
        while (!finished) {
            std::deque<SocketCommand> commands;
            bool stopping = false;
            {
                std::lock_guard lock(connection->mutex);
                commands.swap(connection->commands);
                stopping = connection->stopping;
            }
            if (stopping && commands.empty()) {
                close_code = 1001;
                close_reason = "Host shutdown";
                break;
            }
            for (const SocketCommand& command : commands) {
                size_t sent = 0;
                if (command.close) {
                    std::vector<uint8_t> payload{
                        static_cast<uint8_t>((command.code >> 8) & 0xff),
                        static_cast<uint8_t>(command.code & 0xff)};
                    payload.insert(payload.end(), command.reason.begin(),
                                   command.reason.end());
                    curl_ws_send(socket, payload.data(), payload.size(),
                                 &sent, 0, CURLWS_CLOSE);
                    close_code = command.code;
                    close_reason = command.reason;
                    finished = true;
                    break;
                }
                result = curl_ws_send(
                    socket, command.data.data(), command.data.size(),
                    &sent, 0, CURLWS_TEXT);
                if (result != CURLE_OK && result != CURLE_AGAIN) {
                    close_code = 1006;
                    close_reason = curl_easy_strerror(result);
                    finished = true;
                    break;
                }
            }
            if (finished) break;

            for (;;) {
                size_t received = 0;
                const curl_ws_frame* metadata = nullptr;
                result = curl_ws_recv(
                    socket, receive_buffer.data(), receive_buffer.size(),
                    &received, &metadata);
                if (result == CURLE_AGAIN) break;
                if (result != CURLE_OK) {
                    close_code = 1006;
                    close_reason = curl_easy_strerror(result);
                    finished = true;
                    break;
                }
                if (!metadata) break;
                if ((metadata->flags & CURLWS_CLOSE) != 0) {
                    if (received >= 2) {
                        close_code =
                            (static_cast<int>(receive_buffer[0]) << 8) |
                            receive_buffer[1];
                        close_reason.assign(
                            reinterpret_cast<const char*>(
                                receive_buffer.data() + 2),
                            received - 2);
                    }
                    finished = true;
                    break;
                }
                if ((metadata->flags &
                     (CURLWS_TEXT | CURLWS_BINARY | CURLWS_CONT)) != 0) {
                    incoming.insert(incoming.end(), receive_buffer.begin(),
                                    receive_buffer.begin() +
                                        static_cast<std::ptrdiff_t>(received));
                    if (metadata->bytesleft == 0) {
                        push_socket_event(
                            {CurlResourceLoader::SocketEventType::Message,
                             connection->id,
                             std::string(incoming.begin(), incoming.end()),
                             0, ""});
                        incoming.clear();
                    }
                }
                if (received == 0) break;
            }
            if (!finished)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));
        }

        curl_easy_cleanup(socket);
        push_socket_event(
            {CurlResourceLoader::SocketEventType::Close,
             connection->id, "", close_code, std::move(close_reason)});
    }

    void websocket_command(int id, const std::string& url,
                           const std::string& data, int kind) {
        if (kind == 0) {
            auto connection = std::make_shared<SocketConnection>();
            connection->id = id;
            connection->url = url;
            {
                std::lock_guard lock(sockets_mutex);
                sockets[id] = connection;
            }
            connection->worker =
                std::thread([this, connection] {
                    run_websocket(connection);
                });
            return;
        }

        std::shared_ptr<SocketConnection> connection;
        {
            std::lock_guard lock(sockets_mutex);
            auto found = sockets.find(id);
            if (found == sockets.end()) return;
            connection = found->second;
        }
        SocketCommand command;
        command.data = data;
        if (kind == 2) {
            command.close = true;
            auto [code, reason] = decode_close(data);
            command.code = code;
            command.reason = std::move(reason);
        }
        std::lock_guard lock(connection->mutex);
        connection->commands.push_back(std::move(command));
    }

    bool poll_websocket_event(CurlResourceLoader::SocketEvent& event) {
        {
            std::lock_guard lock(events_mutex);
            if (socket_events.empty()) return false;
            event = std::move(socket_events.front());
            socket_events.pop_front();
        }
        if (event.type == CurlResourceLoader::SocketEventType::Close) {
            std::shared_ptr<SocketConnection> connection;
            {
                std::lock_guard lock(sockets_mutex);
                auto found = sockets.find(event.id);
                if (found != sockets.end()) {
                    connection = found->second;
                    sockets.erase(found);
                }
            }
            if (connection && connection->worker.joinable())
                connection->worker.join();
        }
        return true;
    }

    bool fetch(const network::FetchRequest& request,
               network::FetchResponse& response) {
        response = {};
        error.clear();
        error_buffer[0] = '\0';
        if (!handle) return false;

        std::vector<uint8_t> body;
        network::HeaderMap headers;
        curl_slist* request_headers = nullptr;
        for (const auto& [name, value] : request.headers.entries) {
            const std::string line = name + ": " + value;
            request_headers =
                curl_slist_append(request_headers, line.c_str());
        }

        curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, &headers);
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, request_headers);
        curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, nullptr);
        curl_easy_setopt(handle, CURLOPT_NOBODY, 0L);
        curl_easy_setopt(handle, CURLOPT_HTTPGET, 0L);
        curl_easy_setopt(handle, CURLOPT_POST, 0L);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, nullptr);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(0));

        if (request.method == "GET") {
            curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
        } else if (request.method == "HEAD") {
            curl_easy_setopt(handle, CURLOPT_NOBODY, 1L);
            curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "HEAD");
        } else if (request.method == "POST") {
            curl_easy_setopt(handle, CURLOPT_POST, 1L);
        } else {
            curl_easy_setopt(
                handle, CURLOPT_CUSTOMREQUEST,
                request.method.empty() ? "GET"
                                       : request.method.c_str());
        }
        if (!request.body.empty()) {
            curl_easy_setopt(handle, CURLOPT_POSTFIELDS,
                             request.body.data());
            curl_easy_setopt(
                handle, CURLOPT_POSTFIELDSIZE_LARGE,
                static_cast<curl_off_t>(request.body.size()));
        }
        curl_easy_setopt(handle, CURLOPT_REFERER,
                         request.referrer.empty()
                             ? (referrer.empty() ? nullptr
                                                : referrer.c_str())
                             : request.referrer.c_str());
        curl_easy_setopt(handle, CURLOPT_PROXY,
                         proxy.empty() ? nullptr : proxy.c_str());

        const CURLcode result = curl_easy_perform(handle);
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, nullptr);
        curl_slist_free_all(request_headers);
        if (result != CURLE_OK) {
            error = error_buffer[0] != '\0'
                ? error_buffer
                : curl_easy_strerror(result);
            return false;
        }

        long status = 0;
        char* effective_url = nullptr;
        char* content_type = nullptr;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &effective_url);
        curl_easy_getinfo(handle, CURLINFO_CONTENT_TYPE, &content_type);
        if (content_type && !headers.has("content-type"))
            headers.set("content-type", content_type);

        response.status = static_cast<int32_t>(status);
        response.ok = status >= 200 && status < 300;
        response.headers = std::move(headers);
        response.body = std::move(body);
        response.url =
            effective_url ? effective_url : request.url;
        response.type = network::ResponseType::Basic;
        return true;
    }
};

CurlResourceLoader::CurlResourceLoader() : impl_(std::make_unique<Impl>()) {}
CurlResourceLoader::~CurlResourceLoader() = default;

bool CurlResourceLoader::fetch(const std::string& url,
                               network::FetchResponse& response) {
    network::FetchRequest request;
    request.url = url;
    return impl_->fetch(request, response);
}

bool CurlResourceLoader::fetch(
    const network::FetchRequest& request,
    network::FetchResponse& response) {
    return impl_->fetch(request, response);
}

void CurlResourceLoader::websocket_command(
    int id, const std::string& url, const std::string& data, int kind) {
    impl_->websocket_command(id, url, data, kind);
}

bool CurlResourceLoader::poll_websocket_event(SocketEvent& event) {
    return impl_->poll_websocket_event(event);
}

void CurlResourceLoader::set_referrer(std::string referrer) {
    impl_->referrer = std::move(referrer);
}

void CurlResourceLoader::set_proxy(std::string proxy) {
    impl_->proxy = std::move(proxy);
}

const std::string& CurlResourceLoader::last_error() const noexcept {
    return impl_->error;
}

} // namespace malibu::host
