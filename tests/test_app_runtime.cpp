// tests/test_app_runtime.cpp
// Task 32: Web App Manifest parsing, PWA launch, and Service Worker lifecycle
// + fetch interception backed by the Cache API.

#include <gtest/gtest.h>
#include "malibu/app/manifest.h"
#include "malibu/app/app_runtime.h"
#include "malibu/app/service_worker.h"
#include "malibu/storage/cache_storage.h"

#include <optional>
#include <string>

using namespace malibu::app;

TEST(Manifest, ParsesFields) {
    AppManifest m = parse_manifest(R"({
        "name": "Malibu Notes",
        "short_name": "Notes",
        "start_url": "/index.html",
        "display": "standalone",
        "icons": [
            {"src": "/icon-192.png", "sizes": "192x192", "type": "image/png"},
            {"src": "/icon-512.png", "sizes": "512x512", "type": "image/png"}
        ]
    })");
    ASSERT_TRUE(m.valid) << m.error;
    EXPECT_EQ(m.name, "Malibu Notes");
    EXPECT_EQ(m.short_name, "Notes");
    EXPECT_EQ(m.start_url, "/index.html");
    EXPECT_EQ(m.display, DisplayMode::Standalone);
    EXPECT_TRUE(m.standalone());
    ASSERT_EQ(m.icons.size(), 2u);
    EXPECT_EQ(m.best_icon(192), "/icon-192.png");
    EXPECT_EQ(m.best_icon(300), "/icon-512.png");
}

TEST(Manifest, MissingStartUrlIsInvalid) {
    AppManifest m = parse_manifest(R"({"name":"X","display":"standalone"})");
    EXPECT_FALSE(m.valid);
    EXPECT_NE(m.error.find("start_url"), std::string::npos);
}

TEST(AppRuntime, LaunchesStandalonePwa) {
    AppRuntime rt;
    auto loader = [](const std::string& url) -> std::optional<std::string> {
        if (url == "/index.html") return std::string("<body><h1 id='t'>Hello PWA</h1></body>");
        return std::nullopt;
    };
    auto r = rt.launch(R"({"name":"App","start_url":"/index.html","display":"standalone"})", loader);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_TRUE(r.standalone);
    EXPECT_EQ(r.window_title, "App");
    ASSERT_NE(r.view, nullptr);
    EXPECT_EQ(r.view->eval_js("document.querySelector('#t').textContent"), "\"Hello PWA\"");
}

TEST(AppRuntime, AbortsOnInvalidManifest) {
    AppRuntime rt;
    auto loader = [](const std::string&) -> std::optional<std::string> { return std::string("<body></body>"); };
    auto r = rt.launch(R"({"name":"NoStart"})", loader);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.view, nullptr);
    EXPECT_NE(r.error.find("start_url"), std::string::npos);
}

TEST(AppRuntime, AbortsWhenStartUrlUnloadable) {
    AppRuntime rt;
    auto loader = [](const std::string&) -> std::optional<std::string> { return std::nullopt; };
    auto r = rt.launch(R"({"name":"App","start_url":"/missing"})", loader);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("start_url"), std::string::npos);
}

TEST(ServiceWorker, LifecycleAndInterceptFromCache) {
    malibu::storage::CacheStorage caches;
    ServiceWorkerHost sw(caches);
    ASSERT_TRUE(sw.register_script(R"(
        self.addEventListener('install', function(event) {
            event.waitUntil((async function() {
                var cache = await caches.open('v1');
                await cache.put('/data.json', new Response('{"cached":true}', { status: 200 }));
            })());
        });
        self.addEventListener('fetch', function(event) {
            event.respondWith((async function() {
                var cache = await caches.open('v1');
                var hit = await cache.match(event.request);
                if (hit) return hit;
                return new Response('fallback', { status: 404 });
            })());
        });
    )")) << sw.error();

    sw.install();
    EXPECT_EQ(sw.state(), ServiceWorkerHost::State::Installed);
    sw.activate();
    EXPECT_EQ(sw.state(), ServiceWorkerHost::State::Activated);

    // Cached resource is served from the SW.
    auto r1 = sw.handle_fetch("/data.json");
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->status, 200);
    EXPECT_EQ(r1->body, "{\"cached\":true}");

    // Uncached resource falls back to the SW-generated response.
    auto r2 = sw.handle_fetch("/other");
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->status, 404);
    EXPECT_EQ(r2->body, "fallback");
}

TEST(ServiceWorker, NoRespondWithFallsThrough) {
    malibu::storage::CacheStorage caches;
    ServiceWorkerHost sw(caches);
    ASSERT_TRUE(sw.register_script(
        "self.addEventListener('fetch', function(event) { /* no respondWith */ });"));
    sw.install();
    sw.activate();
    auto r = sw.handle_fetch("/passthrough");
    EXPECT_FALSE(r.has_value());  // caller proceeds to the network
}

TEST(ServiceWorker, RegisterReportsParseError) {
    malibu::storage::CacheStorage caches;
    ServiceWorkerHost sw(caches);
    EXPECT_FALSE(sw.register_script("self.addEventListener('fetch', function( {"));  // syntax error
    EXPECT_EQ(sw.state(), ServiceWorkerHost::State::Failed);
}
