// tests/test_network.cpp
// Task 22: Fetch with CORS, HTTP cache + ETag revalidation, CSP connect-src,
// and connection-error handling — exercised with a mock transport.

#include <gtest/gtest.h>
#include "malibu/network/fetch_engine.h"
#include "malibu/security/csp_enforcer.h"

#include <functional>

using namespace malibu::network;
using malibu::security::Origin;

namespace {
struct MockTransport : Transport {
    std::function<bool(const FetchRequest&, FetchResponse&)> handler;
    int calls = 0;
    bool send(const FetchRequest& req, FetchResponse& out) override {
        ++calls;
        return handler ? handler(req, out) : false;
    }
};
FetchResponse ok_response(int status, const std::string& body) {
    FetchResponse r;
    r.status = status;
    r.body.assign(body.begin(), body.end());
    return r;
}
}  // namespace

TEST(Network, SameOriginGet) {
    MockTransport tx;
    tx.handler = [](const FetchRequest&, FetchResponse& out) { out = ok_response(200, "hello"); return true; };
    FetchEngine engine(tx);
    FetchRequest req; req.url = "https://example.com/data";
    FetchResponse r = engine.fetch(req, Origin::parse("https://example.com"));
    EXPECT_EQ(r.type, ResponseType::Basic);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(std::string(r.body.begin(), r.body.end()), "hello");
}

TEST(Network, CrossOriginWithoutCorsHeadersIsError) {
    MockTransport tx;
    tx.handler = [](const FetchRequest&, FetchResponse& out) { out = ok_response(200, "secret"); return true; };
    FetchEngine engine(tx);
    FetchRequest req; req.url = "https://api.other.com/data";
    FetchResponse r = engine.fetch(req, Origin::parse("https://example.com"));
    EXPECT_EQ(r.type, ResponseType::Error);   // → Fetch Promise rejects with TypeError
    EXPECT_FALSE(r.ok);
}

TEST(Network, CrossOriginWithCorsHeaderAllowed) {
    MockTransport tx;
    tx.handler = [](const FetchRequest&, FetchResponse& out) {
        out = ok_response(200, "ok");
        out.headers.set("Access-Control-Allow-Origin", "https://example.com");
        return true;
    };
    FetchEngine engine(tx);
    FetchRequest req; req.url = "https://api.other.com/data";
    FetchResponse r = engine.fetch(req, Origin::parse("https://example.com"));
    EXPECT_EQ(r.type, ResponseType::Cors);
    EXPECT_TRUE(r.ok);
}

TEST(Network, CorsWildcardAllowed) {
    MockTransport tx;
    tx.handler = [](const FetchRequest&, FetchResponse& out) {
        out = ok_response(200, "ok");
        out.headers.set("Access-Control-Allow-Origin", "*");
        return true;
    };
    FetchEngine engine(tx);
    FetchRequest req; req.url = "https://cdn.other.com/lib.js";
    FetchResponse r = engine.fetch(req, Origin::parse("https://example.com"));
    EXPECT_EQ(r.type, ResponseType::Cors);
}

TEST(Network, ConnectionErrorIsNetworkError) {
    MockTransport tx;
    tx.handler = [](const FetchRequest&, FetchResponse&) { return false; };  // DNS/TCP failure
    FetchEngine engine(tx);
    FetchRequest req; req.url = "https://example.com/x";
    FetchResponse r = engine.fetch(req, Origin::parse("https://example.com"));
    EXPECT_EQ(r.type, ResponseType::Error);
}

TEST(Network, ETagRevalidationServes304FromCache) {
    MockTransport tx;
    bool first = true;
    tx.handler = [&](const FetchRequest& req, FetchResponse& out) {
        if (first) {
            first = false;
            out = ok_response(200, "cached-body");
            out.headers.set("ETag", "\"v1\"");
            out.headers.set("Cache-Control", "no-cache");  // force revalidation next time
            return true;
        }
        // Second request must carry the conditional header.
        EXPECT_EQ(req.headers.get("If-None-Match"), "\"v1\"");
        out.status = 304;
        return true;
    };
    FetchEngine engine(tx);
    Origin origin = Origin::parse("https://example.com");
    FetchRequest req; req.url = "https://example.com/doc";

    FetchResponse r1 = engine.fetch(req, origin);
    EXPECT_EQ(std::string(r1.body.begin(), r1.body.end()), "cached-body");

    FetchResponse r2 = engine.fetch(req, origin);  // revalidates → 304 → cached body
    EXPECT_EQ(std::string(r2.body.begin(), r2.body.end()), "cached-body");
    EXPECT_EQ(tx.calls, 2);
}

TEST(Network, FreshMaxAgeServedWithoutNetwork) {
    MockTransport tx;
    tx.handler = [](const FetchRequest&, FetchResponse& out) {
        out = ok_response(200, "fresh");
        out.headers.set("Cache-Control", "max-age=3600");
        return true;
    };
    FetchEngine engine(tx);
    Origin origin = Origin::parse("https://example.com");
    FetchRequest req; req.url = "https://example.com/asset";
    engine.fetch(req, origin);             // stores in cache
    FetchResponse r = engine.fetch(req, origin);  // served from cache
    EXPECT_EQ(std::string(r.body.begin(), r.body.end()), "fresh");
    EXPECT_EQ(tx.calls, 1);                // no second network request
}

TEST(Network, CspBlocksBeforeConnection) {
    MockTransport tx;
    tx.handler = [](const FetchRequest&, FetchResponse& out) { out = ok_response(200, "x"); return true; };
    malibu::security::CspEnforcer csp;
    csp.set_document_origin(Origin::parse("https://example.com"));
    csp.set_policy("connect-src 'self'");
    FetchEngine engine(tx);
    engine.set_csp(&csp);

    FetchRequest req; req.url = "https://evil.com/track";
    FetchResponse r = engine.fetch(req, Origin::parse("https://example.com"));
    EXPECT_EQ(r.type, ResponseType::Error);
    EXPECT_EQ(tx.calls, 0);                       // blocked before any connection
    EXPECT_EQ(csp.violation_count(), 1u);
}
