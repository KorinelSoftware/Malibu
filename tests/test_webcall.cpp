// tests/test_webcall.cpp
// Task 19 / Property 2: guards gate the fast path; a guard failure deoptimizes
// the call site and the slow path produces an identical observable result.

#include <gtest/gtest.h>
#include "malibu/webcall/webcall_dispatch.h"
#include "malibu/js/bytecode/bytecode.h"
#include "malibu/dom/document.h"

using namespace malibu::webcall;
using malibu::js::bytecode::CallSiteTable;
using malibu::js::bytecode::CallSiteEntry;
using malibu::dom::Document;
using malibu::dom::DOMTree;
using malibu::NodeHandle;

namespace {
struct Fixture {
    Document doc;
    DOMTree  tree{doc};
    RealmState realm;
    WebCallContext ctx;
    CallSiteTable cst;
    NodeHandle app;

    Fixture() {
        ctx.dom = &tree;
        ctx.realm = &realm;
        // Build: root > div#app
        app = tree.create_element(u"div");
        tree.set_attribute(app, u"id", u"app");
        tree.append_child(doc.root(), app);
        // Register two call sites.
        cst.register_function(1, {
            CallSiteEntry{/*id*/10, WEBCALL_DOM_QUERY_SELECTOR, false, 0},
            CallSiteEntry{/*id*/11, WEBCALL_DOM_SET_TEXT_CONTENT, false, 0},
        });
    }
};
}  // namespace

TEST(WebCall, FastPathResolvesNode) {
    Fixture f;
    WebCallArgs args; args.str_a = u"#app";
    ValueBox out;
    MalibuErrorCode rc = dispatch_call_site(f.ctx, f.cst, 10,
                                            encode_handle(f.doc.root()), args, &out);
    EXPECT_EQ(rc, MALIBU_OK);
    EXPECT_EQ(out.kind, ValueBox::Kind::Node);
    EXPECT_EQ(out.node, f.app);
    EXPECT_FALSE(f.cst.is_deoptimized(10));
}

TEST(WebCall, FastAndSlowProduceIdenticalResult) {
    Fixture f;
    WebCallArgs args; args.str_a = u"#app";
    ValueBox fast, slow;
    fast_handler(WEBCALL_DOM_QUERY_SELECTOR)(f.ctx, encode_handle(f.doc.root()), args, &fast);
    slow_handler(WEBCALL_DOM_QUERY_SELECTOR)(f.ctx, encode_handle(f.doc.root()), args, &slow);
    EXPECT_EQ(fast, slow);
    EXPECT_EQ(fast.node, f.app);
}

TEST(WebCall, GuardFailureDeoptimizesAndSlowPathMatches) {
    Fixture f;
    WebCallArgs args; args.str_a = u"#app";

    // First, capture the correct (fast) result.
    ValueBox expected;
    dispatch_call_site(f.ctx, f.cst, 10, encode_handle(f.doc.root()), args, &expected);
    ASSERT_EQ(expected.node, f.app);
    ASSERT_FALSE(f.cst.is_deoptimized(10));

    // Monkey-patch document.querySelector → guard fails on next dispatch.
    f.realm.query_selector_is_original = false;
    ValueBox after;
    MalibuErrorCode rc = dispatch_call_site(f.ctx, f.cst, 10,
                                            encode_handle(f.doc.root()), args, &after);
    EXPECT_EQ(rc, MALIBU_OK);
    EXPECT_TRUE(f.cst.is_deoptimized(10));        // site deoptimized
    EXPECT_EQ(after.node, expected.node);          // identical observable result
}

TEST(WebCall, DeoptimizedSiteGoesStraightToSlowPath) {
    Fixture f;
    WebCallArgs args; args.str_a = u"#app";
    f.cst.deoptimize(10);
    // Even with guards that WOULD pass, a deoptimized site uses the slow path.
    ValueBox out;
    MalibuErrorCode rc = dispatch_call_site(f.ctx, f.cst, 10,
                                            encode_handle(f.doc.root()), args, &out);
    EXPECT_EQ(rc, MALIBU_OK);
    EXPECT_EQ(out.node, f.app);
    EXPECT_TRUE(f.cst.is_deoptimized(10));
}

TEST(WebCall, ReoptimizeReenablesFastPath) {
    Fixture f;
    f.cst.deoptimize(10);
    EXPECT_TRUE(f.cst.is_deoptimized(10));
    f.cst.reoptimize(10);
    EXPECT_FALSE(f.cst.is_deoptimized(10));

    WebCallArgs args; args.str_a = u"#app";
    ValueBox out;
    dispatch_call_site(f.ctx, f.cst, 10, encode_handle(f.doc.root()), args, &out);
    EXPECT_EQ(out.node, f.app);
    EXPECT_FALSE(f.cst.is_deoptimized(10));  // guards passed → stayed optimized
}

TEST(WebCall, SetTextContentMutatesDom) {
    Fixture f;
    WebCallArgs args; args.str_a = u"Hola Malibu";
    ValueBox out;
    MalibuErrorCode rc = dispatch_call_site(f.ctx, f.cst, 11, encode_handle(f.app), args, &out);
    EXPECT_EQ(rc, MALIBU_OK);
    EXPECT_EQ(f.tree.text_content(f.app), std::u16string(u"Hola Malibu"));
}

TEST(WebCall, DeoptEpochAdvancesOnGuardFailure) {
    Fixture f;
    WebCallArgs args; args.str_a = u"#app";
    EXPECT_EQ(f.cst.epoch_of(10), 0u);
    f.realm.has_proxy_intercept = true;  // a different guard fails this time
    ValueBox out;
    dispatch_call_site(f.ctx, f.cst, 10, encode_handle(f.doc.root()), args, &out);
    EXPECT_TRUE(f.cst.is_deoptimized(10));
    EXPECT_EQ(f.cst.epoch_of(10), 1u);
    EXPECT_EQ(out.node, f.app);
}
