// tests/test_html.cpp
// Task 28: HTML tokenizer + tree construction into a NodeTable-backed DOM.

#include <gtest/gtest.h>
#include "malibu/html/html_parser.h"
#include "malibu/dom/document.h"

using namespace malibu::dom;
using malibu::html::HTMLParser;
using malibu::NodeHandle;

TEST(HTMLParser, SimpleElementWithText) {
    Document doc; DOMTree tree(doc);
    HTMLParser p;
    p.parse(u"<h1>Hello</h1>", tree);
    NodeHandle h1 = tree.query_selector(doc.root(), u"h1");
    ASSERT_FALSE(h1.is_null());
    EXPECT_EQ(tree.text_content(h1), std::u16string(u"Hello"));
}

TEST(HTMLParser, NestedStructureAndAttributes) {
    Document doc; DOMTree tree(doc);
    HTMLParser p;
    p.parse(u"<div id='app' class='box'><p>Hi <b>there</b></p></div>", tree);
    NodeHandle app = tree.query_selector(doc.root(), u"#app");
    ASSERT_FALSE(app.is_null());
    EXPECT_EQ(*tree.get_attribute(app, u"class"), std::u16string(u"box"));
    NodeHandle b = tree.query_selector(doc.root(), u"div p b");
    ASSERT_FALSE(b.is_null());
    EXPECT_EQ(tree.text_content(b), std::u16string(u"there"));
    EXPECT_EQ(tree.text_content(app), std::u16string(u"Hi there"));
}

TEST(HTMLParser, VoidElements) {
    Document doc; DOMTree tree(doc);
    HTMLParser p;
    p.parse(u"<div><br><img src='x.png'>after</div>", tree);
    NodeHandle div = tree.query_selector(doc.root(), u"div");
    ASSERT_FALSE(div.is_null());
    // br and img are void: not parents of the following text.
    std::vector<NodeHandle> imgs;
    tree.query_selector_all(doc.root(), u"img", imgs);
    ASSERT_EQ(imgs.size(), 1u);
    EXPECT_EQ(*tree.get_attribute(imgs[0], u"src"), std::u16string(u"x.png"));
    EXPECT_EQ(tree.text_content(div), std::u16string(u"after"));
}

TEST(HTMLParser, CapturesScriptsAndStyles) {
    Document doc; DOMTree tree(doc);
    HTMLParser p;
    auto parsed = p.parse(
        u"<style>.a{color:red}</style><script>var x = 1 < 2;</script>", tree);
    ASSERT_EQ(parsed.stylesheets.size(), 1u);
    ASSERT_EQ(parsed.scripts.size(), 1u);
    EXPECT_EQ(parsed.stylesheets[0], std::u16string(u".a{color:red}"));
    EXPECT_EQ(parsed.scripts[0], std::u16string(u"var x = 1 < 2;"));  // '<' not treated as a tag
}

TEST(HTMLParser, PreservesScriptLoadingAttributes) {
    Document doc; DOMTree tree(doc);
    HTMLParser p;
    auto parsed = p.parse(
        u"<script src='app.js' type='module' async defer nomodule></script>"
        u"<script type='application/ld+json'>{\"name\":\"Malibu\"}</script>",
        tree);
    ASSERT_EQ(parsed.script_items.size(), 2u);
    EXPECT_TRUE(parsed.script_items[0].external);
    EXPECT_TRUE(parsed.script_items[0].async);
    EXPECT_TRUE(parsed.script_items[0].defer);
    EXPECT_TRUE(parsed.script_items[0].no_module);
    EXPECT_EQ(parsed.script_items[0].src, u"app.js");
    EXPECT_EQ(parsed.script_items[0].type, u"module");
    EXPECT_FALSE(parsed.script_items[1].external);
    EXPECT_EQ(parsed.script_items[1].type, u"application/ld+json");
}

TEST(HTMLParser, Entities) {
    Document doc; DOMTree tree(doc);
    HTMLParser p;
    p.parse(u"<p>a &amp; b &lt; c &#65;</p>", tree);
    NodeHandle pn = tree.query_selector(doc.root(), u"p");
    ASSERT_FALSE(pn.is_null());
    EXPECT_EQ(tree.text_content(pn), std::u16string(u"a & b < c A"));
}

TEST(HTMLParser, ImplicitParagraphClose) {
    Document doc; DOMTree tree(doc);
    HTMLParser p;
    p.parse(u"<p>one<p>two", tree);
    std::vector<NodeHandle> ps;
    tree.query_selector_all(doc.root(), u"p", ps);
    ASSERT_EQ(ps.size(), 2u);
    // The second <p> is a sibling, not nested inside the first.
    EXPECT_EQ(tree.parent_node(ps[1]), tree.parent_node(ps[0]));
}

TEST(HTMLParser, Comments) {
    Document doc; DOMTree tree(doc);
    HTMLParser p;
    p.parse(u"<div><!-- ignored -->kept</div>", tree);
    NodeHandle div = tree.query_selector(doc.root(), u"div");
    EXPECT_EQ(tree.text_content(div), std::u16string(u"kept"));
}
