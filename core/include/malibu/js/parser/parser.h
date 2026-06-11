#pragma once
// core/include/malibu/js/parser/parser.h
// ECMAScript parser (Task 14). Hand-written; no dependency on V8/SpiderMonkey/JSC.
//
// This implementation provides a complete ECMAScript tokenizer and a
// recursive-descent parser over a practical subset of ES2022 that produces a
// lightweight AST. On a syntax error it emits a ParseError with source
// location (file, line, column) and a descriptive message, and emits no AST.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <memory>

namespace malibu::js::parser {

struct ParseError {
    std::string file;
    uint32_t    line   = 0;
    uint32_t    column = 0;
    std::string message;
};

// ---------------------------------------------------------------------------
// Token stream
// ---------------------------------------------------------------------------
enum class TokenType : uint8_t {
    EndOfFile,
    Identifier,
    Keyword,
    NumberLiteral,
    StringLiteral,
    TemplateLiteral,
    RegexLiteral,
    Punctuator,
};

struct Token {
    TokenType   type = TokenType::EndOfFile;
    std::string value;     // UTF-8 lexeme (decoded for strings)
    uint32_t    line   = 1;
    uint32_t    column = 1;
};

// ---------------------------------------------------------------------------
// AST — a compact representation sufficient for the bytecode compiler subset.
// ---------------------------------------------------------------------------
enum class NodeKind : uint8_t {
    Program,
    VariableDeclaration,
    FunctionDeclaration,
    ArrowFunction,
    ClassDeclaration,
    Block,
    If, For, While, DoWhile, Switch,
    Return, Throw, Try,
    Break, Continue, Labeled, With,
    ExpressionStatement,
    ImportDeclaration, ExportDeclaration,
    // expressions
    Identifier,
    NumberLiteral, StringLiteral, BoolLiteral, NullLiteral, UndefinedLiteral,
    TemplateLiteral,
    ArrayLiteral, ObjectLiteral,
    Member, Call, New,
    Unary, Binary, Logical, Assignment, Conditional,
    Await, Yield,
    Spread,
};

// Bit flags carried by `Node::flags`. Currently used to annotate class members
// (a Member node inside a ClassDeclaration) and destructuring patterns.
namespace node_flags {
constexpr uint8_t ClassStatic   = 1u << 0;  // `static` member
constexpr uint8_t ClassGetter   = 1u << 1;  // `get` accessor
constexpr uint8_t ClassSetter   = 1u << 2;  // `set` accessor
constexpr uint8_t ClassField    = 1u << 3;  // field (vs method)
constexpr uint8_t Computed      = 1u << 4;  // `[expr]` computed key
constexpr uint8_t Generator     = 1u << 5;  // `function*` / `*method` generator
constexpr uint8_t Optional      = 1u << 6;  // optional chaining link (`?.` / `?.(` / `?.[`)
}  // namespace node_flags

struct Node {
    NodeKind                            kind;
    std::string                         str;        // identifier name / literal text / operator
    std::vector<std::unique_ptr<Node>>  children;
    uint32_t                            line   = 0;
    uint32_t                            column = 0;
    bool                                is_async = false;  // async function / arrow
    uint8_t                             flags    = 0;      // node_flags::* (class members, patterns)

    explicit Node(NodeKind k) : kind(k) {}
};

using NodePtr = std::unique_ptr<Node>;

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
class Parser {
public:
    struct Result {
        NodePtr                 program;  // null if parsing failed
        std::vector<ParseError> errors;
        [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
    };

    // Parses UTF-8 source. `filename` is recorded in any ParseError.
    Result parse(std::string_view source, std::string_view filename);
};

} // namespace malibu::js::parser
