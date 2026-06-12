// tests/test_parser.cpp
// Task 14 (subset): valid ES parses to an AST; a syntax error reports a
// location and emits no program.

#include <gtest/gtest.h>
#include "malibu/js/parser/parser.h"

using namespace malibu::js::parser;

TEST(Parser, ParsesValidProgram) {
    Parser p;
    auto r = p.parse(
        "const x = 1 + 2 * 3;\n"
        "function add(a, b) { return a + b; }\n"
        "let f = (n) => n * 2;\n"
        "if (x > 5) { add(x, f(x)); } else { add(0, 0); }\n",
        "test.js");
    EXPECT_TRUE(r.ok());
    ASSERT_NE(r.program, nullptr);
    EXPECT_EQ(r.program->kind, NodeKind::Program);
    EXPECT_GE(r.program->children.size(), 4u);
}

TEST(Parser, ParsesMemberAndCallChains) {
    Parser p;
    auto r = p.parse("document.querySelector('#app').textContent = 'hi';", "m.js");
    EXPECT_TRUE(r.ok());
    ASSERT_NE(r.program, nullptr);
}

TEST(Parser, ParsesAsyncObjectMethods) {
    Parser p;
    auto r = p.parse(
        "const service = {"
        "  async snapshot(value) { await save(value); },"
        "  async *stream() { yield 1; },"
        "  async() { return 'plain method named async'; },"
        "  async: 42"
        "};",
        "async-object-methods.js");
    EXPECT_TRUE(r.ok());
    ASSERT_NE(r.program, nullptr);
}

TEST(Parser, ParsesTaggedTemplates) {
    Parser p;
    auto r = p.parse(
        "const pattern = String.raw`\\p{Emoji}${suffix}`;"
        "const result = tag`before ${value} after`;",
        "tagged-templates.js");
    EXPECT_TRUE(r.ok());
    ASSERT_NE(r.program, nullptr);
}

TEST(Parser, DistinguishesDynamicImportFromImportDeclarations) {
    Parser parser;
    auto dynamic = parser.parse(
        "import('./feature.js').then(run);", "dynamic-import.js");
    ASSERT_TRUE(dynamic.program);
    EXPECT_TRUE(dynamic.errors.empty());

    auto declaration = parser.parse(
        "import value from './feature.js';", "static-import.js");
    ASSERT_TRUE(declaration.program);
    EXPECT_TRUE(declaration.errors.empty());

    auto meta = parser.parse(
        "const moduleURL = import.meta.url;", "import-meta.js");
    ASSERT_TRUE(meta.program);
    EXPECT_TRUE(meta.errors.empty());
}

TEST(Parser, PreservesEmptyDoWhileBody) {
    Parser p;
    auto r = p.parse("let i = 0; do; while (i++ < 2);", "empty-do-while.js");
    ASSERT_TRUE(r.ok());
    ASSERT_NE(r.program, nullptr);
    ASSERT_EQ(r.program->children.size(), 2u);
    const Node& loop = *r.program->children[1];
    ASSERT_EQ(loop.kind, NodeKind::DoWhile);
    ASSERT_EQ(loop.children.size(), 2u);
    EXPECT_EQ(loop.children[0]->kind, NodeKind::Block);
}

TEST(Parser, AppliesAutomaticSemicolonInsertionAfterReturnNewline) {
    Parser parser;
    auto result = parser.parse(
        "function cleanup(){if(true)return\n"
        "for(const key of keys){use(key)}}",
        "return-asi.js");
    ASSERT_TRUE(result.program);
    EXPECT_TRUE(result.errors.empty());
}

TEST(Parser, PreservesSequenceExpressions) {
    Parser p;
    auto r = p.parse("state.a = 1, state.b = 2, state.ready = true;", "sequence.js");
    ASSERT_TRUE(r.ok());
    ASSERT_NE(r.program, nullptr);
    ASSERT_EQ(r.program->children.size(), 1u);
    const Node& statement = *r.program->children[0];
    ASSERT_EQ(statement.kind, NodeKind::ExpressionStatement);
    ASSERT_EQ(statement.children.size(), 1u);
    EXPECT_EQ(statement.children[0]->kind, NodeKind::Sequence);
    EXPECT_EQ(statement.children[0]->children.size(), 3u);
}

TEST(Parser, WrapsContinuousOptionalChains) {
    Parser p;
    auto r = p.parse("const value = source?.app.getBuildNumber();", "optional-chain.js");
    ASSERT_TRUE(r.ok());
    ASSERT_NE(r.program, nullptr);
}

TEST(Parser, PreservesBigIntLiterals) {
    Parser p;
    auto r = p.parse(
        "const decimal = 123456789012345678901234567890n;"
        "const binary = 0b1010n;"
        "const separated = 1_000_000n;",
        "bigint.js");
    ASSERT_TRUE(r.ok());
    ASSERT_NE(r.program, nullptr);
    ASSERT_EQ(r.program->children.size(), 3u);
    ASSERT_FALSE(r.program->children[0]->children.empty());
    ASSERT_FALSE(r.program->children[0]->children[0]->children.empty());
    EXPECT_EQ(r.program->children[0]->children[0]->children[0]->kind,
              NodeKind::BigIntLiteral);
    EXPECT_EQ(r.program->children[1]->children[0]->children[0]->kind,
              NodeKind::BigIntLiteral);
}

TEST(Parser, RejectsFractionalBigIntLiteral) {
    Parser p;
    auto r = p.parse("1.5n;", "bad-bigint.js");
    EXPECT_FALSE(r.ok());
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors[0].message.find("BigInt"), std::string::npos);
}

TEST(Parser, ReportsSyntaxErrorWithLocationAndNoProgram) {
    Parser p;
    // Missing closing paren on line 3.
    auto r = p.parse("let a = 1;\nlet b = 2;\nfoo(a, b;\n", "bad.js");
    EXPECT_FALSE(r.ok());
    ASSERT_FALSE(r.errors.empty());
    EXPECT_EQ(r.errors[0].file, "bad.js");
    EXPECT_EQ(r.errors[0].line, 3u);
    EXPECT_EQ(r.program, nullptr);  // no AST on failure
}

TEST(Parser, ReportsUnterminatedStringFromLexer) {
    Parser p;
    auto r = p.parse("var s = \"unterminated;\n", "str.js");
    EXPECT_FALSE(r.ok());
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors[0].message.find("string"), std::string::npos);
}
