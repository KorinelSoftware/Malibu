// tests/test_css.cpp
// Tasks 10-11: CSS parser, selector matching/specificity, cascade, inheritance,
// var() resolution, display:none, and invalidation.

#include <gtest/gtest.h>
#include "malibu/css/parser/css_parser.h"
#include "malibu/css/selector/selector.h"
#include "malibu/css/style/style_resolver.h"
#include "malibu/css/invalidation/invalidation.h"
#include "malibu/dom/document.h"

using namespace malibu::css;
using malibu::dom::Document;
using malibu::dom::DOMTree;
using malibu::NodeHandle;

TEST(CSSParser, ParsesRulesAndDeclarations) {
    CSSParser p;
    StyleSheet s = p.parse(u"body { color: red; margin: 0 auto; } .x, #y { display: none !important; }");
    ASSERT_EQ(s.rules.size(), 2u);
    EXPECT_EQ(s.rules[0].selectors.size(), 1u);
    EXPECT_EQ(s.rules[0].declarations.size(), 2u);
    EXPECT_EQ(s.rules[0].declarations[0].property, std::u16string(u"color"));
    EXPECT_EQ(s.rules[0].declarations[0].value, std::u16string(u"red"));
    EXPECT_EQ(s.rules[1].selectors.size(), 2u);  // .x and #y
    EXPECT_TRUE(s.rules[1].declarations[0].important);
}

TEST(CSSParser, DiscardsMalformedDeclarationKeepsRest) {
    CSSParser p;
    StyleSheet s = p.parse(u".a { color: blue; this-is-broken; width: 10px; }");
    ASSERT_EQ(s.rules.size(), 1u);
    // The broken declaration (no colon) is dropped; the two valid ones remain.
    ASSERT_EQ(s.rules[0].declarations.size(), 2u);
    EXPECT_EQ(s.rules[0].declarations[0].property, std::u16string(u"color"));
    EXPECT_EQ(s.rules[0].declarations[1].property, std::u16string(u"width"));
}

TEST(CSSSelector, Specificity) {
    EXPECT_TRUE((parse_selector(u"div").specificity) < (parse_selector(u".cls").specificity));
    EXPECT_TRUE((parse_selector(u".cls").specificity) < (parse_selector(u"#id").specificity));
    auto a = parse_selector(u"div.cls").specificity;     // (0,1,1)
    auto b = parse_selector(u"#id").specificity;          // (1,0,0)
    EXPECT_TRUE(a < b);
}

namespace {
// Build: <body><div id=app class=box><p>text</p></div></body>
struct Tree {
    Document doc; DOMTree tree{doc};
    NodeHandle body, div, p;
    Tree() {
        body = tree.create_element(u"body");
        div = tree.create_element(u"div");
        tree.set_attribute(div, u"id", u"app");
        tree.set_attribute(div, u"class", u"box");
        p = tree.create_element(u"p");
        tree.append_child(doc.root(), body);
        tree.append_child(body, div);
        tree.append_child(div, p);
    }
};
}  // namespace

TEST(CSSStyle, CascadeAndSpecificityWins) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(u"div { color: red; } #app { color: blue; }"), Origin::Author);
    r.resolve(t.doc);
    const ComputedStyle* cs = r.style_for(t.doc, t.div);
    ASSERT_NE(cs, nullptr);
    EXPECT_EQ(cs->color, (Color{0, 0, 255, 255}));  // #app (higher specificity) wins
}

TEST(CSSStyle, ImportantBeatsSpecificity) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(u"#app { color: blue; } div { color: green !important; }"), Origin::Author);
    r.resolve(t.doc);
    EXPECT_EQ(r.style_for(t.doc, t.div)->color, (Color{0, 128, 0, 255}));
}

TEST(CSSStyle, InheritanceThroughLevels) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(u"body { color: red; font-size: 20px; }"), Origin::Author);
    r.resolve(t.doc);
    // <p> three levels down inherits color + font-size.
    const ComputedStyle* cs = r.style_for(t.doc, t.p);
    ASSERT_NE(cs, nullptr);
    EXPECT_EQ(cs->color, (Color{255, 0, 0, 255}));
    EXPECT_FLOAT_EQ(cs->font_size, 20.0f);
}

TEST(CSSStyle, VarResolvesFromNearestAncestor) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(u":root, body { --main: #00ff00; } p { color: var(--main); }"), Origin::Author);
    r.resolve(t.doc);
    EXPECT_EQ(r.style_for(t.doc, t.p)->color, (Color{0, 255, 0, 255}));
}

TEST(CSSStyle, VarFallback) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(u"p { color: var(--missing, blue); }"), Origin::Author);
    r.resolve(t.doc);
    EXPECT_EQ(r.style_for(t.doc, t.p)->color, (Color{0, 0, 255, 255}));
}

TEST(CSSStyle, DisplayNone) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(u"p { display: none; }"), Origin::Author);
    r.resolve(t.doc);
    EXPECT_EQ(r.style_for(t.doc, t.p)->display, DisplayType::None);
}

TEST(CSSStyle, UnsupportedDisplayFallsBackToBlock) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(u"p { display: ruby-text; }"), Origin::Author);
    r.resolve(t.doc);
    EXPECT_EQ(r.style_for(t.doc, t.p)->display, DisplayType::Block);
}

TEST(CSSStyle, InlineStyleHasHighestPriority) {
    Tree t;
    t.tree.set_attribute(t.div, u"style", u"color: orange");
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(u"#app { color: blue !important; }"), Origin::Author);
    r.resolve(t.doc);
    // !important author still beats inline non-important.
    EXPECT_EQ(r.style_for(t.doc, t.div)->color, (Color{0, 0, 255, 255}));
}

TEST(CSSStyle, BoxShorthandAndLengths) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(u"#app { padding: 10px 20px; width: 50%; box-sizing: border-box; }"), Origin::Author);
    r.resolve(t.doc);
    const ComputedStyle* cs = r.style_for(t.doc, t.div);
    EXPECT_FLOAT_EQ(cs->padding.top.value, 10.0f);
    EXPECT_FLOAT_EQ(cs->padding.right.value, 20.0f);
    EXPECT_FLOAT_EQ(cs->padding.bottom.value, 10.0f);
    EXPECT_TRUE(cs->width.is_percent());
    EXPECT_EQ(cs->box_sizing, BoxSizing::BorderBox);
}

TEST(CSSInvalidation, MarkDirtyAndRecompute) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(u".highlight { color: red; }"), Origin::Author);
    r.resolve(t.doc);
    EXPECT_EQ(r.style_for(t.doc, t.div)->color, (Color{0, 0, 0, 255}));  // default black

    // Add the class and invalidate.
    t.tree.set_attribute(t.div, u"class", u"box highlight");
    StyleInvalidator inv;
    inv.mark_dirty(t.div, InvalidationReason::ClassChanged);
    EXPECT_TRUE(inv.is_dirty(t.div));
    inv.recompute(t.doc, r);
    EXPECT_EQ(r.style_for(t.doc, t.div)->color, (Color{255, 0, 0, 255}));
    EXPECT_EQ(inv.dirty_count(), 0u);
}
