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

TEST(CSSSupports, ReportsImplementedPropertiesAndSelectors) {
    EXPECT_TRUE(supports_property_value(u"display", u"grid"));
    EXPECT_TRUE(supports_property_value(u"width", u"calc(100% - 10px)"));
    EXPECT_TRUE(supports_property_value(u"--theme-color", u"#ff00aa"));
    EXPECT_FALSE(supports_property_value(u"display", u"ruby-text"));
    EXPECT_FALSE(supports_property_value(u"backdrop-filter", u"blur(2px)"));
    EXPECT_FALSE(supports_property_value(u"width", u"10px; color:red"));

    EXPECT_TRUE(supports_selector(u".message:is(.new, .read)"));
    EXPECT_TRUE(supports_condition(u"(display: flex) and (position: sticky)"));
    EXPECT_FALSE(supports_condition(u"selector(::-webkit-scrollbar)"));
    EXPECT_FALSE(supports_condition(u"selector(button:has(svg))"));
}

TEST(CSSSelector, Specificity) {
    EXPECT_TRUE((parse_selector(u"div").specificity) < (parse_selector(u".cls").specificity));
    EXPECT_TRUE((parse_selector(u".cls").specificity) < (parse_selector(u"#id").specificity));
    auto a = parse_selector(u"div.cls").specificity;     // (0,1,1)
    auto b = parse_selector(u"#id").specificity;          // (1,0,0)
    EXPECT_TRUE(a < b);
}

TEST(CSSSelector, PreservesBeforeAndAfterPseudoElements) {
    auto before = parse_selector(u"main::before");
    auto after = parse_selector(u".item:after");
    EXPECT_TRUE(before.valid);
    EXPECT_TRUE(after.valid);
    EXPECT_EQ(before.pseudo_element, PseudoElement::Before);
    EXPECT_EQ(after.pseudo_element, PseudoElement::After);
    EXPECT_EQ(before.specificity, (Specificity{0, 0, 2}));
    EXPECT_EQ(after.specificity, (Specificity{0, 1, 1}));
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

TEST(CSSStyle, SvgFillCascadesAndResolvesCurrentColor) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(
        u"body { color: #ff4500; fill: currentColor; }"
        u"p { fill: url(#gradient) white; }"),
        Origin::Author);
    r.resolve(t.doc);
    EXPECT_EQ(r.style_for(t.doc, t.div)->svg_fill,
              (Color{255, 69, 0, 255}));
    EXPECT_EQ(r.style_for(t.doc, t.p)->svg_fill,
              (Color{255, 255, 255, 255}));
}

TEST(CSSStyle, PseudoElementStyleDoesNotLeakOntoOriginatingElement) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(
        u"#app { background-color: blue; }"
        u"#app::before { content:''; background-color:red; "
        u"position:absolute; width:20px; height:20px; }"),
        Origin::Author);
    r.resolve(t.doc);
    const ComputedStyle* normal = r.style_for(t.doc, t.div);
    const ComputedStyle* before =
        r.pseudo_style_for(t.doc, t.div, PseudoElement::Before);
    ASSERT_NE(normal, nullptr);
    ASSERT_NE(before, nullptr);
    EXPECT_EQ(normal->background_color, (Color{0, 0, 255, 255}));
    EXPECT_EQ(before->background_color, (Color{255, 0, 0, 255}));
    EXPECT_TRUE(before->generates_content);
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

TEST(CSSStyle, VarForwardReferencesAreOrderIndependent) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(
        u":root { --foreground: var(--palette-entry); --palette-entry: #123456; }"
        u"p { color: var(--foreground); }"),
        Origin::Author);
    r.resolve(t.doc);
    EXPECT_EQ(r.style_for(t.doc, t.p)->color, (Color{0x12, 0x34, 0x56, 255}));
}

TEST(CSSStyle, VarCycleUsesFallbackAtPointOfUse) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(
        u":root { --a: var(--b); --b: var(--a); }"
        u"p { color: var(--a, rgb(12, 34, 56)); }"),
        Origin::Author);
    r.resolve(t.doc);
    EXPECT_EQ(r.style_for(t.doc, t.p)->color, (Color{12, 34, 56, 255}));
}

TEST(CSSStyle, VarFallbackPreservesNestedCommas) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(
        u"p { color: var(--missing, rgb(17, 34, 51)); }"),
        Origin::Author);
    r.resolve(t.doc);
    EXPECT_EQ(r.style_for(t.doc, t.p)->color, (Color{17, 34, 51, 255}));
}

TEST(CSSStyle, ParsesModernHslWithCalcAndAlpha) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(
        u":root { --saturation-factor: 1; --tone-hsl: 235 "
        u"calc(var(--saturation-factor, 1)*86%) 65%; }"
        u"p { color: hsl(var(--tone-hsl)/0.5); }"),
        Origin::Author);
    r.resolve(t.doc);
    EXPECT_EQ(r.style_for(t.doc, t.p)->color, (Color{89, 102, 243, 128}));
}

TEST(CSSStyle, ParsesModernRgbPercentagesAndSlashAlpha) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(
        u"p { color: rgb(100% 20% 0% / 25%); }"),
        Origin::Author);
    r.resolve(t.doc);
    EXPECT_EQ(r.style_for(t.doc, t.p)->color, (Color{255, 51, 0, 64}));
}

TEST(CSSStyle, ParsesColorMixWithWeightedAlpha) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(
        u"p { color: color-mix(in srgb, rgb(255 0 0 / 50%) 50%, blue 50%); }"),
        Origin::Author);
    r.resolve(t.doc);
    EXPECT_EQ(r.style_for(t.doc, t.p)->color, (Color{85, 0, 170, 192}));
}

TEST(CSSStyle, ColorMixOklabHonorsZeroWeight) {
    Tree t;
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(
        u"p { color: color-mix(in oklab, hsl(0 0% 0% / 12%) 100%, "
        u"hsl(0 0% 0% / 12%) 0%); }"),
        Origin::Author);
    r.resolve(t.doc);
    EXPECT_EQ(r.style_for(t.doc, t.p)->color, (Color{0, 0, 0, 31}));
}

TEST(CSSStyle, ResolvesDiscordStyleColorMixChain) {
    Tree t;
    t.tree.set_attribute(t.div, u"class", u"theme-darker");
    CSSParser parser;
    StyleResolver r;
    r.add_stylesheet(parser.parse(
        u":root { --saturation-factor: 1; "
        u"--opacity-black-12-hsl: 0 calc(var(--saturation-factor, 1)*0%) 0%; }"
        u".theme-darker { --input-background-default:"
        u"color-mix(in oklab,"
        u"hsl(var(--opacity-black-12-hsl)/0.1215686275490196) 100%,"
        u"hsl(var(--custom-theme-base-color-hsl,0 0% 0%)/0.1215686275490196) "
        u"var(--custom-theme-base-color-amount,0%)); }"
        u".theme-darker p { background-color:var(--input-background-default); }"),
        Origin::Author);
    r.resolve(t.doc);
    EXPECT_EQ(r.style_for(t.doc, t.p)->background_color,
              (Color{0, 0, 0, 31}));
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
