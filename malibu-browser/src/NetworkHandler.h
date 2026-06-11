#pragma once
// NetworkHandler: libcurl-based HTTP fetcher for MalibuView request interception.

#include <malibu/network/fetch_types.h>
#include <string>
#include <functional>

class NetworkHandler {
public:
    NetworkHandler();
    ~NetworkHandler();

    // Set the network proxy if needed
    void set_proxy(const std::string& proxy_url);

    // Main fetch function: called by View::RequestHandler
    // Returns true on success (fills 'out'), false on failure
    bool fetch(const std::string& url, malibu::network::FetchResponse& out);

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};
