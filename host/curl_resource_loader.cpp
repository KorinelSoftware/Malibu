#include "malibu/host/curl_resource_loader.h"

#include <curl/curl.h>

#include <cstdlib>
#include <mutex>
#include <string>
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
    CURL* handle = nullptr;
    std::string referrer;
    std::string proxy;
    std::string error;
    char error_buffer[CURL_ERROR_SIZE] = {};

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
        if (handle) curl_easy_cleanup(handle);
    }

    bool fetch(const std::string& url, network::FetchResponse& response) {
        response = {};
        error.clear();
        error_buffer[0] = '\0';
        if (!handle) return false;

        std::vector<uint8_t> body;
        network::HeaderMap headers;
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, &headers);
        curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(handle, CURLOPT_POST, 0L);
        curl_easy_setopt(handle, CURLOPT_REFERER,
                         referrer.empty() ? nullptr : referrer.c_str());
        curl_easy_setopt(handle, CURLOPT_PROXY,
                         proxy.empty() ? nullptr : proxy.c_str());

        const CURLcode result = curl_easy_perform(handle);
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
        response.url = effective_url ? effective_url : url;
        response.type = network::ResponseType::Basic;
        return true;
    }
};

CurlResourceLoader::CurlResourceLoader() : impl_(std::make_unique<Impl>()) {}
CurlResourceLoader::~CurlResourceLoader() = default;

bool CurlResourceLoader::fetch(const std::string& url,
                               network::FetchResponse& response) {
    return impl_->fetch(url, response);
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
