// tests/test_discord_phase2.cpp
// Phase 2 of "run Discord": with the modern-JS language layer working, exercise
// the WEB PLATFORM surface a real client touches at boot — DOM construction,
// events, storage, timers, fetch, and rendering an actual component tree into
// #app-mount. Each probe asserts an exact value so the first real gap surfaces.

#include <gtest/gtest.h>
#include "malibu/view/view.h"

using malibu::view::View;

namespace {
constexpr const char* kShell = R"HTML(
<!doctype html><html><head><title>Discord</title></head>
<body><div id="app-mount"></div></body></html>
)HTML";

::testing::AssertionResult Eval(View& v, const char* label, const std::string& src,
                                const std::string& expected) {
    std::string r = v.eval_js(src);
    if (r == expected) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << "Discord phase2 [" << label << "]: got " << r << ", expected " << expected;
}
}  // namespace

// DOM construction — React's host config calls these to build the tree.
TEST(DiscordPhase2, DomConstruction) {
    View v;
    ASSERT_TRUE(v.load_html(kShell, "https://discord.com/app"));

    EXPECT_TRUE(Eval(v, "createElement+appendChild+textContent", R"JS(
        var mount = document.getElementById('app-mount');
        var div = document.createElement('div');
        div.className = 'app';
        div.textContent = 'Login';
        mount.appendChild(div);
        var found = document.querySelector('.app');
        found.textContent;
    )JS", "\"Login\""));

    EXPECT_TRUE(Eval(v, "setAttribute+getAttribute", R"JS(
        var el = document.createElement('button');
        el.setAttribute('data-id', '42');
        el.id = 'submit';
        el.getAttribute('data-id') + ':' + el.id;
    )JS", "\"42:submit\""));
}

// Event listeners on built elements (Discord wires onClick everywhere).
TEST(DiscordPhase2, Events) {
    View v;
    ASSERT_TRUE(v.load_html(kShell, "https://discord.com/app"));
    EXPECT_TRUE(Eval(v, "addEventListener+dispatch", R"JS(
        var b = document.createElement('button');
        b.id = 'go';
        document.getElementById('app-mount').appendChild(b);
        globalThis.clicks = 0;
        b.addEventListener('click', function(){ globalThis.clicks++; });
        b.dispatchEvent(new Event('click'));
        b.dispatchEvent(new Event('click'));
        globalThis.clicks;
    )JS", "2"));
}

// Storage — Discord caches the auth token + settings in localStorage.
TEST(DiscordPhase2, Storage) {
    View v;
    ASSERT_TRUE(v.load_html(kShell, "https://discord.com/app"));
    EXPECT_TRUE(Eval(v, "localStorage", R"JS(
        localStorage.setItem('token', 'abc123');
        localStorage.setItem('count', '7');
        localStorage.getItem('token') + ':' + localStorage.getItem('count') + ':' + localStorage.length;
    )JS", "\"abc123:7:2\""));
}

// Timers + microtasks — the scheduler and async flows.
TEST(DiscordPhase2, Timers) {
    View v;
    ASSERT_TRUE(v.load_html(kShell, "https://discord.com/app"));
    v.eval_js(R"JS(
        globalThis.ticks = 0;
        setTimeout(function(){ globalThis.ticks += 1; }, 0);
        Promise.resolve().then(function(){ globalThis.ticks += 10; });
    )JS");
    v.run_tasks();
    EXPECT_TRUE(Eval(v, "setTimeout+microtask", "globalThis.ticks", "11"));
}

// fetch -> Response.json(), intercepted (no network). Discord's API layer.
TEST(DiscordPhase2, Fetch) {
    View v;
    v.set_request_handler([](const std::string& url, malibu::network::FetchResponse& out) {
        if (url.find("/api/v9/users/@me") != std::string::npos) {
            std::string json = "{\"id\":\"1\",\"username\":\"seage\"}";
            out.status = 200;
            out.ok = true;
            out.type = malibu::network::ResponseType::Basic;
            out.body.assign(json.begin(), json.end());
            return true;
        }
        return false;
    });
    ASSERT_TRUE(v.load_html(kShell, "https://discord.com/app"));
    v.eval_js(R"JS(
        globalThis.who = '';
        fetch('https://discord.com/api/v9/users/@me')
            .then(function(r){ return r.json(); })
            .then(function(u){ globalThis.who = u.username; });
    )JS");
    v.run_tasks();
    EXPECT_TRUE(Eval(v, "fetch+json", "globalThis.who", "\"seage\""));
}
