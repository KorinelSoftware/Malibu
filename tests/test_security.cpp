// tests/test_security.cpp
// Task 13: Origin parsing, Same-Origin Policy, CSP enforcement.

#include <gtest/gtest.h>
#include "malibu/security/origin.h"
#include "malibu/security/same_origin_policy.h"
#include "malibu/security/csp_enforcer.h"

using namespace malibu::security;

TEST(Origin, ParseAndDefaults) {
    Origin a = Origin::parse("https://example.com/path?q=1");
    EXPECT_EQ(a.scheme, "https");
    EXPECT_EQ(a.host, "example.com");
    EXPECT_EQ(a.port, 443);
    EXPECT_EQ(a.serialize(), "https://example.com");

    Origin b = Origin::parse("http://example.com:8080/");
    EXPECT_EQ(b.port, 8080);
    EXPECT_EQ(b.serialize(), "http://example.com:8080");
}

TEST(Origin, SameOriginRules) {
    Origin a = Origin::parse("https://example.com");
    Origin b = Origin::parse("https://example.com/other");
    Origin c = Origin::parse("https://example.com:444");
    Origin d = Origin::parse("http://example.com");
    Origin e = Origin::parse("https://evil.com");
    EXPECT_TRUE(a.same_origin(b));
    EXPECT_FALSE(a.same_origin(c));   // different port
    EXPECT_FALSE(a.same_origin(d));   // different scheme
    EXPECT_FALSE(a.same_origin(e));   // different host
}

TEST(Origin, OpaqueNeverSameOrigin) {
    Origin o1 = Origin::parse("data:text/html,hi");
    Origin o2 = Origin::parse("about:blank");
    EXPECT_TRUE(o1.is_opaque());
    EXPECT_TRUE(o2.is_opaque());
    EXPECT_FALSE(o1.same_origin(o1));   // opaque is never same-origin, even with itself
    EXPECT_FALSE(o1.same_origin(o2));
    EXPECT_EQ(o1.serialize(), "null");
}

TEST(SameOriginPolicy, ThrowsOnCrossOrigin) {
    Origin a = Origin::parse("https://example.com");
    Origin b = Origin::parse("https://evil.com");
    EXPECT_THROW(SameOriginPolicy::check_same_origin(a, b), SecurityError);
    EXPECT_NO_THROW(SameOriginPolicy::check_same_origin(a, Origin::parse("https://example.com/x")));
    EXPECT_FALSE(SameOriginPolicy::is_allowed(a, b));
}

TEST(Csp, NoPolicyAllowsEverything) {
    CspEnforcer csp;
    EXPECT_TRUE(csp.allows(CspEnforcer::Directive::ScriptSrc, "https://anywhere.com/x.js"));
    EXPECT_FALSE(csp.has_policy());
}

TEST(Csp, SelfAndHostMatching) {
    CspEnforcer csp;
    csp.set_document_origin(Origin::parse("https://example.com"));
    csp.set_policy("default-src 'self'; img-src 'self' *.cdn.com; connect-src https://api.example.com");

    // script falls back to default-src 'self'
    EXPECT_TRUE(csp.allows(CspEnforcer::Directive::ScriptSrc, "https://example.com/app.js"));
    EXPECT_FALSE(csp.allows(CspEnforcer::Directive::ScriptSrc, "https://evil.com/app.js"));

    // img-src self + wildcard host
    EXPECT_TRUE(csp.allows(CspEnforcer::Directive::ImgSrc, "https://images.cdn.com/a.png"));
    EXPECT_TRUE(csp.allows(CspEnforcer::Directive::ImgSrc, "https://example.com/logo.png"));
    EXPECT_FALSE(csp.allows(CspEnforcer::Directive::ImgSrc, "https://other.com/a.png"));

    // connect-src explicit origin
    EXPECT_TRUE(csp.allows(CspEnforcer::Directive::ConnectSrc, "https://api.example.com/v1"));
    EXPECT_FALSE(csp.allows(CspEnforcer::Directive::ConnectSrc, "https://example.com/v1"));
}

TEST(Csp, NoneBlocksAll) {
    CspEnforcer csp;
    csp.set_policy("default-src 'none'");
    EXPECT_FALSE(csp.allows(CspEnforcer::Directive::ScriptSrc, "https://example.com/x.js"));
}

TEST(Csp, SchemeSource) {
    CspEnforcer csp;
    csp.set_policy("img-src https:");
    EXPECT_TRUE(csp.allows(CspEnforcer::Directive::ImgSrc, "https://anywhere.com/a.png"));
    EXPECT_FALSE(csp.allows(CspEnforcer::Directive::ImgSrc, "http://anywhere.com/a.png"));
}

TEST(Csp, ViolationRecorded) {
    CspEnforcer csp;
    csp.set_document_origin(Origin::parse("https://example.com"));
    csp.set_policy("connect-src 'self'; report-uri /csp-report");
    const char* url = "https://evil.com/track";
    if (!csp.allows(CspEnforcer::Directive::ConnectSrc, url)) {
        csp.report_violation(CspEnforcer::Directive::ConnectSrc, url, "https://example.com/");
    }
    EXPECT_EQ(csp.violation_count(), 1u);
    EXPECT_EQ(csp.report_uri(), "/csp-report");
}
