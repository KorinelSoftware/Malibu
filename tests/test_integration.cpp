// tests/test_integration.cpp
// Full-engine integration: HTML -> DOM -> CSS -> JS (mutates DOM via WebCall)
// -> style invalidation -> layout -> raster. The complete Malibu pipeline.

#include <gtest/gtest.h>
#include "malibu/html/html_parser.h"
#include "malibu/css/parser/css_parser.h"
#include "malibu/css/style/style_resolver.h"
#include "malibu/css/invalidation/invalidation.h"
#include "malibu/layout/layout_engine.h"
#include "malibu/render/renderer.h"
#include "malibu/js/engine.h"
#include "malibu/js/dom_binding.h"
#include "malibu/dom/document.h"

using namespace malibu;

TEST(Integration, ParseStyleLayoutRender) {
    // 1. Parse HTML into the DOM.
    dom::Document doc;
    dom::DOMTree tree(doc);
    html::HTMLParser hp;
    auto parsed = hp.parse(
        u"<style>#app{background-color:#ff0000;width:50px;height:50px}</style>"
        u"<body><div id='app'></div></body>", tree);
    ASSERT_EQ(parsed.stylesheets.size(), 1u);

    // 2. Resolve CSS (user-agent + page <style>).
    css::CSSParser cssp;
    css::StyleResolver resolver;
    resolver.add_stylesheet(cssp.parse(css::user_agent_css()), css::Origin::UserAgent);
    resolver.add_stylesheet(cssp.parse(parsed.stylesheets[0]), css::Origin::Author);
    resolver.resolve(doc);

    NodeHandle app = tree.query_selector(doc.root(), u"#app");
    ASSERT_FALSE(app.is_null());

    // 3. Layout + render → the box is red.
    layout::LayoutEngine engine;
    render::Renderer renderer;
    layout::LayoutBox* root = engine.layout_document(doc, 100, 100);
    render::Framebuffer fb = renderer.render(doc, root, 100, 100, {255, 255, 255, 255});
    // body has 8px UA margin → #app at (8,8).
    EXPECT_EQ(fb.at(40, 40), (render::Color{255, 0, 0, 255}));
}

TEST(Integration, JavaScriptDrivesReRender) {
    dom::Document doc;
    dom::DOMTree tree(doc);
    html::HTMLParser hp;
    auto parsed = hp.parse(
        u"<style>"
        u"#app{background-color:#ff0000;width:50px;height:50px}"
        u"#app.green{background-color:#00ff00}"
        u"</style>"
        u"<body><div id='app'></div></body>", tree);

    css::CSSParser cssp;
    css::StyleResolver resolver;
    resolver.add_stylesheet(cssp.parse(css::user_agent_css()), css::Origin::UserAgent);
    resolver.add_stylesheet(cssp.parse(parsed.stylesheets[0]), css::Origin::Author);
    resolver.resolve(doc);

    layout::LayoutEngine engine;
    render::Renderer renderer;

    // Initial render → red.
    {
        layout::LayoutBox* root = engine.layout_document(doc, 100, 100);
        render::Framebuffer fb = renderer.render(doc, root, 100, 100, {255, 255, 255, 255});
        EXPECT_EQ(fb.at(40, 40), (render::Color{255, 0, 0, 255}));
    }

    // 4. Run JavaScript that mutates the DOM through the WebCall ABI.
    js::Engine jsengine;
    js::DomBinding binding(jsengine.interpreter(), tree, doc.root());
    binding.install();
    auto r = jsengine.evaluate(
        "var el = document.querySelector('#app');"
        "el.setAttribute('class', 'green');"
        "el.textContent = 'hi';");
    ASSERT_TRUE(r.ok) << r.error;

    // 5. Invalidate + re-resolve styles for the changed subtree.
    NodeHandle app = tree.query_selector(doc.root(), u"#app");
    css::StyleInvalidator inv;
    inv.mark_dirty(app, css::InvalidationReason::ClassChanged);
    inv.recompute(doc, resolver);

    // 6. Re-layout + re-render → now green (JS changed the class).
    {
        layout::LayoutBox* root = engine.layout_document(doc, 100, 100);
        render::Framebuffer fb = renderer.render(doc, root, 100, 100, {255, 255, 255, 255});
        EXPECT_EQ(fb.at(40, 40), (render::Color{0, 255, 0, 255}));
    }

    // The text content set by JS is in the real DOM tree.
    EXPECT_EQ(tree.text_content(app), std::u16string(u"hi"));
}
