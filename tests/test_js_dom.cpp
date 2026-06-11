// tests/test_js_dom.cpp
// The Malibu thesis end-to-end: real JavaScript source manipulates the live
// DOM, routed through the WebCall ABI (guards + deopt).

#include <gtest/gtest.h>
#include "malibu/js/engine.h"
#include "malibu/js/dom_binding.h"
#include "malibu/dom/document.h"

using malibu::js::Engine;
using malibu::js::DomBinding;
using malibu::dom::Document;
using malibu::dom::DOMTree;
using malibu::NodeHandle;

namespace {
struct Fixture {
    Engine   engine;
    Document doc;
    DOMTree  tree{doc};
    NodeHandle app;
    std::unique_ptr<DomBinding> binding;

    Fixture() {
        app = tree.create_element(u"div");
        tree.set_attribute(app, u"id", u"app");
        tree.append_child(doc.root(), app);
        binding = std::make_unique<DomBinding>(engine.interpreter(), tree, doc.root());
        binding->install();
    }
};
}  // namespace

TEST(JSDom, QuerySelectorAndSetTextContentFromJS) {
    Fixture f;
    auto r = f.engine.evaluate(
        "let el = document.querySelector('#app');"
        "el.textContent = 'Hola Malibu';"
        "el.textContent");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(f.engine.interpreter().to_string(r.value), u"Hola Malibu");
    // The real DOM tree was mutated through the WebCall ABI.
    EXPECT_EQ(f.tree.text_content(f.app), std::u16string(u"Hola Malibu"));
}

TEST(JSDom, CreateElementAppendChildFromJS) {
    Fixture f;
    auto r = f.engine.evaluate(
        "let span = document.createElement('span');"
        "span.textContent = 'child';"
        "let app = document.querySelector('#app');"
        "app.appendChild(span);"
        "app.querySelectorAll('span').length");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(f.engine.interpreter().to_string(r.value), u"1");
}

TEST(JSDom, GetSetAttributeFromJS) {
    Fixture f;
    auto r = f.engine.evaluate(
        "let el = document.querySelector('#app');"
        "el.setAttribute('data-x', '42');"
        "el.getAttribute('data-x')");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(f.engine.interpreter().to_string(r.value), u"42");
}

TEST(JSDom, IdPropertyReflectsAttribute) {
    Fixture f;
    auto r = f.engine.evaluate("document.querySelector('#app').id");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(f.engine.interpreter().to_string(r.value), u"app");
}

TEST(JSDom, TagNameUppercase) {
    Fixture f;
    auto r = f.engine.evaluate("document.querySelector('#app').tagName");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(f.engine.interpreter().to_string(r.value), u"DIV");
}
