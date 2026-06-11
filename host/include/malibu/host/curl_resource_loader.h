#pragma once

#include <memory>
#include <string>

#include "malibu/network/fetch_types.h"

namespace malibu::host {

// Stateful host-side HTTP(S) transport. A loader reuses its libcurl connection
// pool and cookie engine across navigations and subresource requests.
class CurlResourceLoader {
public:
    CurlResourceLoader();
    ~CurlResourceLoader();

    CurlResourceLoader(const CurlResourceLoader&) = delete;
    CurlResourceLoader& operator=(const CurlResourceLoader&) = delete;

    bool fetch(const std::string& url, network::FetchResponse& response);
    void set_referrer(std::string referrer);
    void set_proxy(std::string proxy);

    [[nodiscard]] const std::string& last_error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace malibu::host
