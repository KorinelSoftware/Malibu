// tests/test_storage.cpp
// Task 24: per-origin isolation, IndexedDB, Cache API, cookies (RFC 6265),
// atomic clear_origin.

#include <gtest/gtest.h>
#include "malibu/storage/storage_engine.h"
#include "malibu/security/origin.h"

using namespace malibu::storage;
using malibu::security::Origin;

namespace {
Origin A() { return Origin::parse("https://a.example.com"); }
Origin B() { return Origin::parse("https://b.example.com"); }
}  // namespace

TEST(Storage, LocalStorageRoundTrip) {
    StorageEngine eng;
    eng.local_storage(A()).set_item("k", "v");
    auto got = eng.local_storage(A()).get_item("k");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "v");
    EXPECT_EQ(eng.local_storage(A()).length(), 1u);
    EXPECT_EQ(*eng.local_storage(A()).key(0), "k");
}

TEST(Storage, LocalStoragePerOriginIsolation) {
    StorageEngine eng;
    eng.local_storage(A()).set_item("secret", "fromA");
    // A different origin sees nothing.
    EXPECT_FALSE(eng.local_storage(B()).get_item("secret").has_value());
    EXPECT_EQ(eng.local_storage(B()).length(), 0u);
}

TEST(Storage, SessionStorageIsolatedBySession) {
    StorageEngine eng;
    eng.session_storage(A(), "s1").set_item("x", "1");
    EXPECT_FALSE(eng.session_storage(A(), "s2").get_item("x").has_value());
    EXPECT_EQ(*eng.session_storage(A(), "s1").get_item("x"), "1");
}

TEST(Storage, IndexedDBPerOrigin) {
    StorageEngine eng;
    eng.indexed_db(A()).open_database("db", 1);
    eng.indexed_db(A()).create_store("db", "store");
    eng.indexed_db(A()).put("db", "store", "key", "value");
    EXPECT_EQ(*eng.indexed_db(A()).get("db", "store", "key"), "value");
    // Other origin: database not openable / not present.
    EXPECT_FALSE(eng.indexed_db(B()).has("db"));
    EXPECT_FALSE(eng.indexed_db(B()).get("db", "store", "key").has_value());
}

TEST(Storage, CacheStoragePerOrigin) {
    StorageEngine eng;
    CacheStorage::CachedResponse r;
    r.status = 200;
    r.body = {'h', 'i'};
    eng.cache_storage(A()).put("v1", "https://a.example.com/x", r);
    auto m = eng.cache_storage(A()).match("v1", "https://a.example.com/x");
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->status, 200);
    EXPECT_FALSE(eng.cache_storage(B()).match("v1", "https://a.example.com/x").has_value());
}

TEST(Storage, CookieHttpOnlyNotInDocumentCookie) {
    StorageEngine eng;
    Cookie c; c.name = "sid"; c.value = "abc"; c.http_only = true;
    eng.cookies().set_cookie("https://a.example.com/", c);
    Cookie c2; c2.name = "theme"; c2.value = "dark";
    eng.cookies().set_cookie("https://a.example.com/", c2);
    std::string doc = eng.cookies().get_cookie_string("https://a.example.com/");
    EXPECT_EQ(doc.find("sid"), std::string::npos);   // HttpOnly excluded
    EXPECT_NE(doc.find("theme=dark"), std::string::npos);
}

TEST(Storage, CookieSecureNotSentOverHttp) {
    StorageEngine eng;
    Cookie c; c.name = "s"; c.value = "1"; c.secure = true;
    eng.cookies().set_cookie("https://a.example.com/", c);
    // Over HTTP, the Secure cookie is excluded.
    EXPECT_TRUE(eng.cookies().get_cookie_string("http://a.example.com/").empty());
    EXPECT_NE(eng.cookies().get_cookie_string("https://a.example.com/").find("s=1"), std::string::npos);
}

TEST(Storage, CookieSameSiteStrictNotSentCrossSite) {
    StorageEngine eng;
    Cookie c; c.name = "ss"; c.value = "1"; c.same_site = Cookie::SameSite::Strict;
    eng.cookies().set_cookie("https://a.example.com/", c);
    CookieStore::Context cross{true, /*same_site=*/false, false};
    EXPECT_TRUE(eng.cookies().get_cookies("https://a.example.com/", cross).empty());
    CookieStore::Context same{true, /*same_site=*/true, false};
    EXPECT_EQ(eng.cookies().get_cookies("https://a.example.com/", same).size(), 1u);
}

TEST(Storage, DocumentCookieParsesAttributesAndRejectsForeignDomains) {
    StorageEngine eng;
    EXPECT_TRUE(eng.cookies().set_cookie_from_document(
        "https://a.example.com/account/page",
        "theme=dark; Path=/; SameSite=Strict; Secure"));
    EXPECT_EQ(eng.cookies().get_cookie_string(
                  "https://a.example.com/other"),
              "theme=dark");
    EXPECT_FALSE(eng.cookies().set_cookie_from_document(
        "https://a.example.com/", "stolen=1; Domain=evil.example"));
    EXPECT_EQ(eng.cookies().size(), 1u);
}

TEST(Storage, DocumentCookieMaxAgeDeletesMatchingCookie) {
    StorageEngine eng;
    EXPECT_TRUE(eng.cookies().set_cookie_from_document(
        "https://a.example.com/", "temporary=1; Path=/"));
    EXPECT_EQ(eng.cookies().get_cookie_string("https://a.example.com/"),
              "temporary=1");
    EXPECT_TRUE(eng.cookies().set_cookie_from_document(
        "https://a.example.com/", "temporary=gone; Path=/; Max-Age=0"));
    EXPECT_TRUE(
        eng.cookies().get_cookie_string("https://a.example.com/").empty());
}

TEST(Storage, ClearOriginRemovesAllTypes) {
    StorageEngine eng;
    eng.local_storage(A()).set_item("k", "v");
    eng.session_storage(A(), "s").set_item("k", "v");
    eng.indexed_db(A()).open_database("db", 1);
    eng.cache_storage(A()).put("c", "u", {});
    Cookie ck; ck.name = "n"; ck.value = "v";
    eng.cookies().set_cookie("https://a.example.com/", ck);

    EXPECT_TRUE(eng.clear_origin(A()));
    EXPECT_EQ(eng.local_storage(A()).length(), 0u);
    EXPECT_FALSE(eng.indexed_db(A()).has("db"));
    EXPECT_TRUE(eng.cache_storage(A()).empty());
    EXPECT_TRUE(eng.cookies().get_cookie_string("https://a.example.com/").empty());
}
