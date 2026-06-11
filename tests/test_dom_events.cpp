// tests/test_dom_events.cpp
// DOM Event model (WHATWG): addEventListener / removeEventListener / Event /
// dispatchEvent with capture → target → bubble, preventDefault,
// stopPropagation, target/currentTarget, and host-initiated dispatch.

#include <gtest/gtest.h>
#include "malibu/view/view.h"

using malibu::view::View;

namespace {
void load_page(View& v) {
    v.load_html("<body><div id='parent'><button id='btn'>x</button></div></body>",
                "https://example.com/");
}
}  // namespace

TEST(DomEvents, AddListenerAndDispatch) {
    View v; load_page(v);
    auto r = v.eval_js(
        "(function(){ globalThis.n = 0;"
        " var b = document.querySelector('#btn');"
        " b.addEventListener('click', function(){ globalThis.n++; });"
        " b.dispatchEvent(new Event('click'));"
        " return globalThis.n; })()");
    EXPECT_EQ(r, "1");
}

TEST(DomEvents, BubblingTargetThenParent) {
    View v; load_page(v);
    auto r = v.eval_js(
        "(function(){ globalThis.log = '';"
        " var p = document.querySelector('#parent'), b = document.querySelector('#btn');"
        " p.addEventListener('click', function(){ globalThis.log += 'P'; });"
        " b.addEventListener('click', function(){ globalThis.log += 'B'; });"
        " b.dispatchEvent(new Event('click', { bubbles: true }));"
        " return globalThis.log; })()");
    EXPECT_EQ(r, "\"BP\"");
}

TEST(DomEvents, CaptureBeforeTarget) {
    View v; load_page(v);
    auto r = v.eval_js(
        "(function(){ globalThis.log = '';"
        " var p = document.querySelector('#parent'), b = document.querySelector('#btn');"
        " p.addEventListener('click', function(){ globalThis.log += 'Pc'; }, true);"
        " b.addEventListener('click', function(){ globalThis.log += 'B'; });"
        " b.dispatchEvent(new Event('click', { bubbles: true }));"
        " return globalThis.log; })()");
    EXPECT_EQ(r, "\"PcB\"");
}

TEST(DomEvents, StopPropagation) {
    View v; load_page(v);
    auto r = v.eval_js(
        "(function(){ globalThis.log = '';"
        " var p = document.querySelector('#parent'), b = document.querySelector('#btn');"
        " p.addEventListener('click', function(){ globalThis.log += 'P'; });"
        " b.addEventListener('click', function(e){ globalThis.log += 'B'; e.stopPropagation(); });"
        " b.dispatchEvent(new Event('click', { bubbles: true }));"
        " return globalThis.log; })()");
    EXPECT_EQ(r, "\"B\"");
}

TEST(DomEvents, PreventDefaultReturnsFalse) {
    View v; load_page(v);
    auto r = v.eval_js(
        "(function(){ var b = document.querySelector('#btn');"
        " b.addEventListener('click', function(e){ e.preventDefault(); });"
        " return b.dispatchEvent(new Event('click', { cancelable: true })); })()");
    EXPECT_EQ(r, "false");
}

TEST(DomEvents, RemoveEventListener) {
    View v; load_page(v);
    auto r = v.eval_js(
        "(function(){ globalThis.n = 0;"
        " function h(){ globalThis.n++; }"
        " var b = document.querySelector('#btn');"
        " b.addEventListener('click', h);"
        " b.removeEventListener('click', h);"
        " b.dispatchEvent(new Event('click'));"
        " return globalThis.n; })()");
    EXPECT_EQ(r, "0");
}

TEST(DomEvents, TargetAndCurrentTarget) {
    View v; load_page(v);
    auto r = v.eval_js(
        "(function(){ globalThis.out = '';"
        " var p = document.querySelector('#parent'), b = document.querySelector('#btn');"
        " p.addEventListener('click', function(e){ globalThis.out = e.target.id + '/' + e.currentTarget.id; });"
        " b.dispatchEvent(new Event('click', { bubbles: true }));"
        " return globalThis.out; })()");
    EXPECT_EQ(r, "\"btn/parent\"");
}

TEST(DomEvents, HostInitiatedDispatch) {
    // Simulates a window click flowing into the DOM via View::dispatch_event.
    View v; load_page(v);
    v.eval_js(
        "globalThis.hostClicks = 0;"
        "document.querySelector('#btn').addEventListener('click', function(){ globalThis.hostClicks++; });");
    EXPECT_TRUE(v.dispatch_event("#btn", "click"));
    EXPECT_EQ(v.eval_js("globalThis.hostClicks"), "1");
}

TEST(DomEvents, DuplicateListenerAddedOnce) {
    View v; load_page(v);
    auto r = v.eval_js(
        "(function(){ globalThis.n = 0;"
        " function h(){ globalThis.n++; }"
        " var b = document.querySelector('#btn');"
        " b.addEventListener('click', h);"
        " b.addEventListener('click', h);"   // identical → ignored per DOM spec
        " b.dispatchEvent(new Event('click'));"
        " return globalThis.n; })()");
    EXPECT_EQ(r, "1");
}
