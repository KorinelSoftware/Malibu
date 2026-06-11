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
