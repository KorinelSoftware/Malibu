// tests/test_layout.cpp
// Task 12: block stacking, box-sizing, display:none exclusion, flex, inline.

#include <gtest/gtest.h>
#include "malibu/dom/document.h"
#include "malibu/css/parser/css_parser.h"
#include "malibu/css/style/style_resolver.h"
#include "malibu/layout/layout_engine.h"

using namespace malibu::dom;
using namespace malibu::css;
using namespace malibu::layout;
using malibu::NodeHandle;

namespace {
// Builds a styled document and lays it out; gives access to boxes.
struct Scene {
    Document      doc;
    DOMTree       tree{doc};
    StyleResolver resolver;
    LayoutEngine  engine;

    void style(const char16_t* author_css) {
        CSSParser p;
        resolver.add_stylesheet(p.parse(user_agent_css()), Origin::UserAgent);
        resolver.add_stylesheet(p.parse(author_css), Origin::Author);
        resolver.resolve(doc);
    }
    LayoutBox* run(float w = 800, float h = 600) { return engine.layout_document(doc, w, h); }
    LayoutBox* box(NodeHandle n) { return engine.box_for_node(n); }
};
}  // namespace

TEST(Layout, BlockStacking) {
    Scene s;
    NodeHandle body = s.tree.create_element(u"body");
    s.tree.append_child(s.doc.root(), body);
    NodeHandle a = s.tree.create_element(u"div"), b = s.tree.create_element(u"div"), c = s.tree.create_element(u"div");
    s.tree.set_attribute(a, u"id", u"a"); s.tree.set_attribute(b, u"id", u"b"); s.tree.set_attribute(c, u"id", u"c");
    s.tree.append_child(body, a); s.tree.append_child(body, b); s.tree.append_child(body, c);

    s.style(u"#a{height:50px} #b{height:30px} #c{height:20px}");
    s.run();
    // body margin 8 → content origin y = 8; blocks stack.
    EXPECT_FLOAT_EQ(s.box(a)->y, 8.0f);
    EXPECT_FLOAT_EQ(s.box(b)->y, 58.0f);
    EXPECT_FLOAT_EQ(s.box(c)->y, 88.0f);
    EXPECT_FLOAT_EQ(s.box(a)->height, 50.0f);
}

TEST(Layout, BorderBoxWidth) {
    Scene s;
    NodeHandle body = s.tree.create_element(u"body");
    s.tree.append_child(s.doc.root(), body);
    NodeHandle d = s.tree.create_element(u"div");
    s.tree.set_attribute(d, u"id", u"d");
    s.tree.append_child(body, d);

    s.style(u"#d{ width:100px; box-sizing:border-box; padding:10px; border:5px; }");
    s.run();
    // content width = 100 - 2*padding(10) - 2*border(5) = 70
    EXPECT_FLOAT_EQ(s.box(d)->width, 70.0f);
}

TEST(Layout, ContentBoxWidthDefault) {
    Scene s;
    NodeHandle body = s.tree.create_element(u"body");
    s.tree.append_child(s.doc.root(), body);
    NodeHandle d = s.tree.create_element(u"div");
    s.tree.set_attribute(d, u"id", u"d");
    s.tree.append_child(body, d);

    s.style(u"#d{ width:100px; padding:10px; }");
    s.run();
    EXPECT_FLOAT_EQ(s.box(d)->width, 100.0f);  // content-box: width is the content
}

TEST(Layout, DisplayNoneExcluded) {
    Scene s;
    NodeHandle body = s.tree.create_element(u"body");
    s.tree.append_child(s.doc.root(), body);
    NodeHandle vis = s.tree.create_element(u"div"), hid = s.tree.create_element(u"div");
    s.tree.set_attribute(vis, u"id", u"vis"); s.tree.set_attribute(hid, u"id", u"hid");
    s.tree.append_child(body, vis); s.tree.append_child(body, hid);

    s.style(u"#hid{ display:none } #vis{ height:10px }");
    s.run();
    EXPECT_NE(s.box(vis), nullptr);
    EXPECT_EQ(s.box(hid), nullptr);  // excluded from layout tree
}

TEST(Layout, FlexSpaceBetween) {
    Scene s;
    NodeHandle row = s.tree.create_element(u"div");
    s.tree.set_attribute(row, u"id", u"row");
    s.tree.append_child(s.doc.root(), row);
    NodeHandle c0 = s.tree.create_element(u"div"), c1 = s.tree.create_element(u"div"), c2 = s.tree.create_element(u"div");
    for (auto c : {c0, c1, c2}) { s.tree.set_attribute(c, u"class", u"item"); s.tree.append_child(row, c); }

    s.style(u"#row{ display:flex; width:300px; justify-content:space-between; } .item{ width:50px; height:20px; }");
    s.run();
    EXPECT_FLOAT_EQ(s.box(c0)->x, 0.0f);
    EXPECT_FLOAT_EQ(s.box(c1)->x, 125.0f);  // 50 + (300-150)/2 gap
    EXPECT_FLOAT_EQ(s.box(c2)->x, 250.0f);
}

TEST(Layout, FlexGrowDistributesSpace) {
    Scene s;
    NodeHandle row = s.tree.create_element(u"div");
    s.tree.set_attribute(row, u"id", u"row");
    s.tree.append_child(s.doc.root(), row);
    NodeHandle c0 = s.tree.create_element(u"div"), c1 = s.tree.create_element(u"div");
    for (auto c : {c0, c1}) { s.tree.set_attribute(c, u"class", u"item"); s.tree.append_child(row, c); }

    s.style(u"#row{ display:flex; width:300px } .item{ flex-grow:1; height:20px }");
    s.run();
    EXPECT_FLOAT_EQ(s.box(c0)->width, 150.0f);
    EXPECT_FLOAT_EQ(s.box(c1)->width, 150.0f);
    EXPECT_FLOAT_EQ(s.box(c0)->x, 0.0f);
    EXPECT_FLOAT_EQ(s.box(c1)->x, 150.0f);
}

TEST(Layout, FlexColumn) {
    Scene s;
    NodeHandle col = s.tree.create_element(u"div");
    s.tree.set_attribute(col, u"id", u"col");
    s.tree.append_child(s.doc.root(), col);
    NodeHandle c0 = s.tree.create_element(u"div"), c1 = s.tree.create_element(u"div");
    for (auto c : {c0, c1}) { s.tree.set_attribute(c, u"class", u"item"); s.tree.append_child(col, c); }

    s.style(u"#col{ display:flex; flex-direction:column; width:100px } .item{ height:30px }");
    s.run();
    EXPECT_FLOAT_EQ(s.box(c0)->y, 0.0f);
    EXPECT_FLOAT_EQ(s.box(c1)->y, 30.0f);
}

TEST(Layout, InlineTextProducesBox) {
    Scene s;
    NodeHandle p = s.tree.create_element(u"p");
    s.tree.append_child(s.doc.root(), p);
    NodeHandle text = s.tree.create_text_node(u"Hello");
    s.tree.append_child(p, text);

    s.style(u"p{ font-size:16px }");
    s.run();
    LayoutBox* tb = s.box(p);
    ASSERT_NE(tb, nullptr);
    EXPECT_GT(tb->height, 0.0f);  // the paragraph has a line of text
}

TEST(Layout, InlineTextWraps) {
    Scene s;
    NodeHandle p = s.tree.create_element(u"p");
    s.tree.set_attribute(p, u"id", u"p");
    s.tree.append_child(s.doc.root(), p);
    s.tree.append_child(p, s.tree.create_text_node(u"one two three four five six seven eight nine ten"));

    s.style(u"#p{ width:60px; font-size:16px }");  // narrow → must wrap to many lines
    s.run();
    LayoutBox* pb = s.box(p);
    ASSERT_NE(pb, nullptr);
    // A single line would be ~16*1.2 = 19.2px tall; wrapping makes it taller.
    EXPECT_GT(pb->height, 40.0f);
}
