// tests/test_render.cpp
// Tasks 25-26: display-list ordering, software rasterization (real pixels),
// glyph-atlas shelf packing + LRU eviction, and Vulkan device bring-up.

#include <gtest/gtest.h>
#include "malibu/dom/document.h"
#include "malibu/css/parser/css_parser.h"
#include "malibu/css/style/style_resolver.h"
#include "malibu/layout/layout_engine.h"
#include "malibu/render/renderer.h"
#include "malibu/render/atlas/glyph_cache.h"

using namespace malibu::dom;
using namespace malibu::css;
using namespace malibu::layout;
using namespace malibu::render;
using malibu::NodeHandle;

namespace {
struct Scene {
    Document doc; DOMTree tree{doc};
    StyleResolver resolver; LayoutEngine engine;
    void style(const char16_t* css) {
        CSSParser p;
        resolver.add_stylesheet(p.parse(user_agent_css()), Origin::UserAgent);
        resolver.add_stylesheet(p.parse(css), Origin::Author);
        resolver.resolve(doc);
    }
    LayoutBox* run(float w, float h) { return engine.layout_document(doc, w, h); }
};
}  // namespace

TEST(Render, DisplayListSortedByZIndexThenOrder) {
    Scene s;
    NodeHandle body = s.tree.create_element(u"body");
    s.tree.append_child(s.doc.root(), body);
    NodeHandle a = s.tree.create_element(u"div"), b = s.tree.create_element(u"div");
    s.tree.set_attribute(a, u"id", u"a"); s.tree.set_attribute(b, u"id", u"b");
    s.tree.append_child(body, a); s.tree.append_child(body, b);
    // a appears first in document order but has higher z-index → painted last.
    s.style(u"#a{ background-color:red; height:10px; z-index:5; position:relative }"
            u"#b{ background-color:blue; height:10px; z-index:1; position:relative }");
    LayoutBox* root = s.run(100, 100);

    Renderer r;
    DisplayList list = r.build_display_list(s.doc, root);
    ASSERT_GE(list.items().size(), 2u);
    // Back-to-front: lower z-index first.
    EXPECT_LT(list.items().front().z_index, list.items().back().z_index);
}

TEST(Render, SoftwareRasterizerFillsBackground) {
    Scene s;
    NodeHandle body = s.tree.create_element(u"body");
    s.tree.append_child(s.doc.root(), body);
    NodeHandle d = s.tree.create_element(u"div");
    s.tree.set_attribute(d, u"id", u"d");
    s.tree.append_child(body, d);
    s.style(u"body{ margin:0 } #d{ background-color:#ff0000; width:50px; height:50px }");
    LayoutBox* root = s.run(100, 100);

    Renderer r;
    Framebuffer fb = r.render(s.doc, root, 100, 100, {255, 255, 255, 255});
    EXPECT_EQ(fb.at(25, 25), (Color{255, 0, 0, 255}));   // inside the red box
    EXPECT_EQ(fb.at(75, 75), (Color{255, 255, 255, 255})); // outside → white background
}

TEST(Render, OpacityBlends) {
    Scene s;
    NodeHandle body = s.tree.create_element(u"body");
    s.tree.append_child(s.doc.root(), body);
    NodeHandle d = s.tree.create_element(u"div");
    s.tree.set_attribute(d, u"id", u"d");
    s.tree.append_child(body, d);
    s.style(u"body{ margin:0 } #d{ background-color:#000000; opacity:0.5; width:50px; height:50px }");
    LayoutBox* root = s.run(100, 100);

    Renderer r;
    Framebuffer fb = r.render(s.doc, root, 100, 100, {255, 255, 255, 255});
    Color c = fb.at(10, 10);
    // 50% black over white ≈ mid grey.
    EXPECT_NEAR(c.r, 128, 4);
    EXPECT_NEAR(c.g, 128, 4);
    EXPECT_NEAR(c.b, 128, 4);
}

TEST(Render, ZOrderCompositing) {
    Scene s;
    NodeHandle body = s.tree.create_element(u"body");
    s.tree.append_child(s.doc.root(), body);
    NodeHandle back = s.tree.create_element(u"div"), front = s.tree.create_element(u"div");
    s.tree.set_attribute(back, u"id", u"back"); s.tree.set_attribute(front, u"id", u"front");
    s.tree.append_child(body, back); s.tree.append_child(body, front);
    s.style(u"body{margin:0} div{position:absolute; width:50px; height:50px; top:0; left:0}"
            u"#back{ background-color:#ff0000; z-index:1 }"
            u"#front{ background-color:#0000ff; z-index:2 }");
    LayoutBox* root = s.run(100, 100);
    // Note: this engine lays both at (0,0) in flow; overlapping fill — front (blue) wins.
    Renderer r;
    Framebuffer fb = r.render(s.doc, root, 100, 100, {255, 255, 255, 255});
    EXPECT_EQ(fb.at(5, 5), (Color{0, 0, 255, 255}));
}

TEST(Render, DescendantCannotEscapeAncestorStackingContext) {
    Scene s;
    NodeHandle body = s.tree.create_element(u"body");
    s.tree.append_child(s.doc.root(), body);
    NodeHandle low = s.tree.create_element(u"div");
    NodeHandle high_child = s.tree.create_element(u"div");
    NodeHandle high = s.tree.create_element(u"div");
    s.tree.set_attribute(low, u"id", u"low");
    s.tree.set_attribute(high_child, u"id", u"high-child");
    s.tree.set_attribute(high, u"id", u"high");
    s.tree.append_child(body, low);
    s.tree.append_child(low, high_child);
    s.tree.append_child(body, high);
    s.style(
        u"body{margin:0}"
        u"#low,#high,#high-child{position:absolute;top:0;left:0;width:40px;height:40px}"
        u"#low{z-index:1;background:red}"
        u"#high-child{z-index:999;background:green}"
        u"#high{z-index:2;background:blue}");

    LayoutBox* root = s.run(60, 60);
    Renderer r;
    Framebuffer fb =
        r.render(s.doc, root, 60, 60, {255, 255, 255, 255});
    EXPECT_EQ(fb.at(10, 10), (Color{0, 0, 255, 255}));
}

TEST(GlyphCache, ShelfPackingAllocates) {
    GlyphCache cache(64, 64);
    AtlasRect r1 = cache.get_or_insert({0, 'A'}, 10, 12, 1);
    AtlasRect r2 = cache.get_or_insert({0, 'B'}, 10, 12, 2);
    EXPECT_EQ(cache.size(), 2u);
    EXPECT_NE(r1.x, r2.x);  // different slots on the same shelf
    EXPECT_EQ(r1.y, r2.y);
    // Re-fetching is a cache hit (same rect, no growth).
    AtlasRect r1b = cache.get_or_insert({0, 'A'}, 10, 12, 3);
    EXPECT_EQ(r1b.x, r1.x);
    EXPECT_EQ(cache.size(), 2u);
}

TEST(GlyphCache, LRUEvictionWhenFull) {
    GlyphCache cache(16, 16);  // tiny atlas: fits one 16x16 glyph at a time
    cache.get_or_insert({0, 'A'}, 16, 16, 1);
    EXPECT_TRUE(cache.contains({0, 'A'}));
    cache.get_or_insert({0, 'B'}, 16, 16, 2);  // forces eviction of LRU 'A'
    EXPECT_TRUE(cache.contains({0, 'B'}));
    EXPECT_FALSE(cache.contains({0, 'A'}));
    EXPECT_GE(cache.eviction_count(), 1u);
}
