// NetworkHandler.cpp: libcurl implementation

#include "NetworkHandler.h"

#include <curl/curl.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

struct NetworkHandler::Impl {
    CURL* curl = nullptr;
    std::string proxy_url;

    Impl() {
        curl = curl_easy_init();
        if (curl) {
            // Follow redirects
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            // Max redirects
            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
            // Timeout
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
            // User agent
            curl_easy_setopt(curl, CURLOPT_USERAGENT,
                "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 MalibuBrowser/1.0");
            // Accept encoding
            curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
            // Write callback
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
            // Header callback
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        }
    }

    ~Impl() {
        if (curl) curl_easy_cleanup(curl);
    }

    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
        auto* data = static_cast<std::vector<uint8_t>*>(userp);
        size_t realsize = size * nmemb;
        data->insert(data->end(),
            static_cast<uint8_t*>(contents),
            static_cast<uint8_t*>(contents) + realsize);
        return realsize;
    }

    static size_t header_callback(void* contents, size_t size, size_t nmemb, void* userp) {
        auto* headers = static_cast<malibu::network::HeaderMap*>(userp);
        size_t realsize = size * nmemb;
        std::string header(static_cast<char*>(contents), realsize);
        // Remove trailing \r\n
        if (header.size() >= 2 && header.substr(header.size() - 2) == "\r\n") {
            header.resize(header.size() - 2);
        }
        // Parse "Key: Value"
        size_t colon = header.find(':');
        if (colon != std::string::npos) {
            std::string key = header.substr(0, colon);
            std::string value = header.substr(colon + 1);
            // Trim leading space from value
            size_t start = value.find_first_not_of(" \t");
            if (start != std::string::npos) value = value.substr(start);
            headers->set(key, value);
        }
        return realsize;
    }

    bool perform_request(const std::string& url, malibu::network::FetchResponse& out) {
        if (!curl) return false;

        std::vector<uint8_t> body;
        malibu::network::HeaderMap headers;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);

        if (!proxy_url.empty()) {
            curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url.c_str());
        } else {
            curl_easy_setopt(curl, CURLOPT_PROXY, "");
        }

        // Reset for each request
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl, CURLOPT_POST, 0L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            return false;
        }

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        char* effective_url = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);

        out.status = static_cast<int32_t>(http_code);
        out.ok = (http_code >= 200 && http_code < 300);
        out.body = std::move(body);
        out.headers = std::move(headers);
        out.url = effective_url ? effective_url : url;
        out.type = malibu::network::ResponseType::Basic;

        return true;
    }
};

NetworkHandler::NetworkHandler() : pimpl_(std::make_unique<Impl>()) {}
NetworkHandler::~NetworkHandler() = default;

void NetworkHandler::set_proxy(const std::string& proxy_url) {
    pimpl_->proxy_url = proxy_url;
}

bool NetworkHandler::fetch(const std::string& url, malibu::network::FetchResponse& out) {
    return pimpl_->perform_request(url, out);
}
