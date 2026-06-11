// js/parser/parser.cpp
// ECMAScript tokenizer + recursive-descent parser (subset) producing an AST.
// On a syntax error: records a ParseError with location and returns no program.

#include "malibu/js/parser/parser.h"

#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace malibu::js::parser {
namespace {

const std::unordered_set<std::string>& keywords() {
    static const std::unordered_set<std::string> kw = {
        "var", "let", "const", "function", "return", "if", "else", "for",
        "while", "do", "switch", "case", "default", "break", "continue",
        "new", "delete", "typeof", "instanceof", "in", "of", "void", "this",
        "class", "extends", "super", "import", "export",
        "try", "catch", "finally", "throw", "async", "await", "yield", "with",
        "true", "false", "null", "undefined",
    };
    return kw;
}

bool is_id_start(char16_t c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c > 127; }
bool is_id_part(char16_t c)  { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c > 127; }

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}
// Char-based regex-vs-division heuristic (for scanning template interpolations):
// `/` begins a regex unless the previous significant char is a value/closer.
bool regex_after(char last) {
    if (std::isalnum(static_cast<unsigned char>(last))) return false;
    return !(last == ')' || last == ']' || last == '_' || last == '$' ||
             last == '.' || last == '"' || last == '\'' || last == '`');
}
void utf8_append(std::string& s, uint32_t cp) {
    if (cp < 0x80) s.push_back(static_cast<char>(cp));
    else if (cp < 0x800) { s.push_back(static_cast<char>(0xC0 | (cp >> 6))); s.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { s.push_back(static_cast<char>(0xE0 | (cp >> 12))); s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); s.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else { s.push_back(static_cast<char>(0xF0 | (cp >> 18))); s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); s.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
}

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------
class Lexer {
public:
    Lexer(std::string_view src, std::string_view file) : src_(src), file_(file) {}

    // Lex the whole stream. Returns false and fills `err` on a lexical error.
    bool tokenize(std::vector<Token>& out, ParseError& err) {
        // Hashbang comment: a leading `#!` line is ignored (only at file start).
        if (src_.size() >= 2 && src_[0] == '#' && src_[1] == '!') {
            while (pos_ < src_.size() && src_[pos_] != '\n') advance();
        }
        while (true) {
            skip_trivia();
            if (pos_ >= src_.size()) {
                out.push_back(Token{TokenType::EndOfFile, "", line_, col_});
                return true;
            }
            Token tok;
            tok.line = line_;
            tok.column = col_;
            char c = src_[pos_];

            if (is_id_start(static_cast<unsigned char>(c)) || (c == '\\' && peek(1) == 'u') ||
                (c == '#' && (is_id_start(static_cast<unsigned char>(peek(1))) || peek(1) == '\\'))) {
                lex_identifier(tok);
            } else if (std::isdigit(static_cast<unsigned char>(c)) ||
                       (c == '.' && pos_ + 1 < src_.size() &&
                        std::isdigit(static_cast<unsigned char>(src_[pos_ + 1])))) {
                if (!lex_number(tok, err)) return false;
            } else if (c == '"' || c == '\'') {
                if (!lex_string(tok, err)) return false;
            } else if (c == '`') {
                if (!lex_template(tok, err)) return false;
            } else if (c == '/' && regex_allowed(out)) {
                if (!lex_regex(tok, err)) return false;
            } else {
                lex_punctuator(tok);
            }
            out.push_back(std::move(tok));
        }
    }

private:
    char peek(size_t off = 0) const {
        return pos_ + off < src_.size() ? src_[pos_ + off] : '\0';
    }
    char advance() {
        char c = src_[pos_++];
        if (c == '\n') { line_++; col_ = 1; } else { col_++; }
        return c;
    }

    void skip_trivia() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
                advance();
            } else if (c == '/' && peek(1) == '/') {
                while (pos_ < src_.size() && src_[pos_] != '\n') advance();
            } else if (c == '/' && peek(1) == '*') {
                advance(); advance();
                while (pos_ < src_.size() && !(src_[pos_] == '*' && peek(1) == '/')) advance();
                if (pos_ < src_.size()) { advance(); advance(); }
            } else {
                break;
            }
        }
    }

    // A '/' starts a regex literal when an expression (not a binary operator) is
    // expected — i.e. the previous token is not a value or a closing bracket.
    bool regex_allowed(const std::vector<Token>& out) const {
        if (out.empty()) return true;
        const Token& last = out.back();
        switch (last.type) {
            case TokenType::Identifier:
            case TokenType::NumberLiteral:
            case TokenType::StringLiteral:
            case TokenType::TemplateLiteral:
            case TokenType::RegexLiteral:
                return false;
            case TokenType::Keyword:
                // value keywords => division follows; statement keywords => regex
                return !(last.value == "this" || last.value == "super" ||
                         last.value == "true" || last.value == "false" || last.value == "null");
            case TokenType::Punctuator:
                return !(last.value == ")" || last.value == "]");
            default:
                return true;
        }
    }

    bool lex_regex(Token& tok, ParseError& err) {
        advance();  // opening '/'
        std::string pattern;
        bool in_class = false;
        while (pos_ < src_.size()) {
            char ch = src_[pos_];
            if (ch == '\n') { err = make_err(tok, "unterminated regular expression"); return false; }
            if (ch == '\\') { pattern.push_back(advance()); if (pos_ < src_.size()) pattern.push_back(advance()); continue; }
            if (ch == '[') in_class = true;
            else if (ch == ']') in_class = false;
            else if (ch == '/' && !in_class) break;
            pattern.push_back(advance());
        }
        if (pos_ >= src_.size()) { err = make_err(tok, "unterminated regular expression"); return false; }
        advance();  // closing '/'
        std::string flags;
        while (pos_ < src_.size() && is_id_part(static_cast<unsigned char>(src_[pos_]))) flags.push_back(advance());
        tok.type = TokenType::RegexLiteral;
        tok.value = pattern + '\0' + flags;  // NUL separates pattern from flags
        return true;
    }

    static int hexval(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    }
    static void append_utf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) s.push_back(static_cast<char>(cp));
        else if (cp < 0x800) { s.push_back(static_cast<char>(0xC0 | (cp >> 6))); s.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { s.push_back(static_cast<char>(0xE0 | (cp >> 12))); s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); s.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else { s.push_back(static_cast<char>(0xF0 | (cp >> 18))); s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); s.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    }

    void lex_identifier(Token& tok) {
        std::string s;
        if (pos_ < src_.size() && src_[pos_] == '#') s.push_back(advance());  // private name (#x)
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == '\\' && peek(1) == 'u') {  // \uXXXX or \u{XXXXX} in an identifier
                advance(); advance();
                uint32_t cp = 0;
                if (peek() == '{') { advance(); while (pos_ < src_.size() && src_[pos_] != '}') cp = cp * 16 + hexval(advance()); if (peek() == '}') advance(); }
                else { for (int i = 0; i < 4 && pos_ < src_.size(); ++i) cp = cp * 16 + hexval(advance()); }
                append_utf8(s, cp);
            } else if (is_id_part(static_cast<unsigned char>(c))) {
                s.push_back(advance());
            } else break;
        }
        tok.value = s;
        tok.type = keywords().count(s) ? TokenType::Keyword : TokenType::Identifier;
    }

    bool lex_number(Token& tok, ParseError& err) {
        std::string s;
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' ||
                              peek(1) == 'b' || peek(1) == 'B' ||
                              peek(1) == 'o' || peek(1) == 'O')) {
            s.push_back(advance()); // 0
            s.push_back(advance()); // x/b/o
            while (pos_ < src_.size() &&
                   (std::isalnum(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '_')) {
                s.push_back(advance());
            }
        } else {
            bool seen_dot = false, seen_exp = false;
            while (pos_ < src_.size()) {
                char c = src_[pos_];
                if (std::isdigit(static_cast<unsigned char>(c)) || c == '_') {
                    s.push_back(advance());
                } else if (c == '.' && !seen_dot && !seen_exp) {
                    seen_dot = true; s.push_back(advance());
                } else if ((c == 'e' || c == 'E') && !seen_exp) {
                    seen_exp = true; s.push_back(advance());
                    if (peek() == '+' || peek() == '-') s.push_back(advance());
                } else {
                    break;
                }
            }
        }
        if (s.empty()) {
            err = make_err(tok, "invalid numeric literal");
            return false;
        }
        if (pos_ < src_.size() && src_[pos_] == 'n') advance();  // BigInt suffix (treated as Number)
        tok.value = s;
        tok.type = TokenType::NumberLiteral;
        return true;
    }

    bool lex_string(Token& tok, ParseError& err) {
        char quote = advance();
        std::string s;
        while (true) {
            if (pos_ >= src_.size()) { err = make_err(tok, "unterminated string literal"); return false; }
            char c = src_[pos_];
            if (c == '\n') { err = make_err(tok, "unterminated string literal"); return false; }
            advance();
            if (c == quote) break;
            if (c == '\\') {
                if (pos_ >= src_.size()) { err = make_err(tok, "unterminated escape"); return false; }
                char e = advance();
                switch (e) {
                    case 'n': s.push_back('\n'); break;
                    case 't': s.push_back('\t'); break;
                    case 'r': s.push_back('\r'); break;
                    case '\\': s.push_back('\\'); break;
                    case '\'': s.push_back('\''); break;
                    case '"': s.push_back('"'); break;
                    case '`': s.push_back('`'); break;
                    case '0': s.push_back('\0'); break;
                    default: s.push_back(e); break; // keep \x, \u sequences literally enough
                }
            } else {
                s.push_back(c);
            }
        }
        tok.value = s;
        tok.type = TokenType::StringLiteral;
        return true;
    }

    // Captures a template literal's raw inner text (between the outer backticks),
    // PRESERVING `${...}` interpolations (with nested templates/strings/braces) so
    // the parser can split and parse them. The outer backticks are not included.
    bool lex_template(Token& tok, ParseError& err) {
        advance(); // opening backtick
        std::string s;
        while (true) {
            if (pos_ >= src_.size()) { err = make_err(tok, "unterminated template literal"); return false; }
            char c = src_[pos_];
            if (c == '`') { advance(); break; }
            if (c == '\\') { s.push_back(advance()); if (pos_ < src_.size()) s.push_back(advance()); continue; }
            if (c == '$' && peek(1) == '{') { s.push_back(advance()); s.push_back(advance()); tmpl_copy_interp(s); continue; }
            s.push_back(advance());
        }
        tok.value = s;
        tok.type = TokenType::TemplateLiteral;
        return true;
    }
    // After `${`: copy through the matching `}` (tracking nested braces, strings,
    // and nested template literals).
    void tmpl_copy_interp(std::string& s) {
        int depth = 1;
        char last = '(';  // interpolation begins in expression position
        while (pos_ < src_.size() && depth > 0) {
            char c = src_[pos_];
            if (c == '\\') { s.push_back(advance()); if (pos_ < src_.size()) s.push_back(advance()); last = 'x'; continue; }
            if (c == '`') { s.push_back(advance()); tmpl_copy_nested(s); last = '`'; continue; }
            if (c == '"' || c == '\'') { s.push_back(advance()); tmpl_copy_str(s, c); last = c; continue; }
            if (c == '/' && peek(1) != '/' && peek(1) != '*' && regex_after(last)) { s.push_back(advance()); tmpl_copy_regex(s); last = '/'; continue; }
            if (c == '{') { ++depth; s.push_back(advance()); last = '{'; continue; }
            if (c == '}') { --depth; s.push_back(advance()); if (depth == 0) return; last = '}'; continue; }
            if (!std::isspace(static_cast<unsigned char>(c))) last = c;
            s.push_back(advance());
        }
    }
    void tmpl_copy_regex(std::string& s) {  // after opening '/'
        bool in_class = false;
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == '\n') return;
            if (c == '\\') { s.push_back(advance()); if (pos_ < src_.size()) s.push_back(advance()); continue; }
            if (c == '[') in_class = true;
            else if (c == ']') in_class = false;
            else if (c == '/' && !in_class) { s.push_back(advance()); break; }
            s.push_back(advance());
        }
        while (pos_ < src_.size() && std::isalnum(static_cast<unsigned char>(src_[pos_]))) s.push_back(advance());
    }
    void tmpl_copy_nested(std::string& s) {  // opening backtick already appended
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == '\\') { s.push_back(advance()); if (pos_ < src_.size()) s.push_back(advance()); continue; }
            if (c == '`') { s.push_back(advance()); return; }
            if (c == '$' && peek(1) == '{') { s.push_back(advance()); s.push_back(advance()); tmpl_copy_interp(s); continue; }
            s.push_back(advance());
        }
    }
    void tmpl_copy_str(std::string& s, char q) {  // opening quote already appended
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == '\\') { s.push_back(advance()); if (pos_ < src_.size()) s.push_back(advance()); continue; }
            s.push_back(advance());
            if (c == q) return;
        }
    }

    void lex_punctuator(Token& tok) {
        // Greedily match the longest known multi-char punctuator.
        static const char* multis[] = {
            ">>>=", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "?\?=",
            "...",
            "==", "!=", "<=", ">=", "&&", "||", "?\?", "?.", "=>", "++", "--",
            "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "**",
        };
        for (const char* m : multis) {
            size_t len = std::char_traits<char>::length(m);
            if (src_.compare(pos_, len, m) == 0) {
                // `?.` is NOT optional chaining when followed by a digit (e.g.
                // `cond ? .5 : .6`) — emit a bare `?` instead.
                if (m[0] == '?' && m[1] == '.' && len == 2 &&
                    pos_ + 2 < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_ + 2])))
                    continue;
                for (size_t i = 0; i < len; ++i) advance();
                tok.value.assign(m, len);
                tok.type = TokenType::Punctuator;
                return;
            }
        }
        tok.value.push_back(advance());
        tok.type = TokenType::Punctuator;
    }

    ParseError make_err(const Token& tok, std::string_view msg) const {
        return ParseError{std::string(file_), tok.line, tok.column, std::string(msg)};
    }

    std::string_view src_;
    std::string_view file_;
    size_t   pos_  = 0;
    uint32_t line_ = 1;
    uint32_t col_  = 1;
};

// ---------------------------------------------------------------------------
// Parser over the token stream.
// ---------------------------------------------------------------------------
class TokenParser {
public:
    TokenParser(std::vector<Token> toks, std::string_view file)
        : toks_(std::move(toks)), file_(file) {}

    Parser::Result run() {
        Parser::Result result;
        auto program = std::make_unique<Node>(NodeKind::Program);
        while (!at_end()) {
            auto stmt = parse_statement();
            if (failed_) { result.errors.push_back(error_); return result; }
            if (stmt) program->children.push_back(std::move(stmt));
        }
        result.program = std::move(program);
        return result;
    }

private:
    const Token& cur() const { return toks_[idx_]; }
    const Token& peek(size_t off = 1) const {
        size_t i = idx_ + off;
        return i < toks_.size() ? toks_[i] : toks_.back();
    }
    bool at_end() const { return cur().type == TokenType::EndOfFile; }
    const Token& advance() { return toks_[idx_ < toks_.size() - 1 ? idx_++ : idx_]; }

    bool is_punct(std::string_view p) const {
        return cur().type == TokenType::Punctuator && cur().value == p;
    }
    bool is_kw(std::string_view k) const {
        return cur().type == TokenType::Keyword && cur().value == k;
    }
    bool accept_punct(std::string_view p) { if (is_punct(p)) { advance(); return true; } return false; }
    bool accept_kw(std::string_view k)    { if (is_kw(k))    { advance(); return true; } return false; }

    void fail(std::string_view msg) {
        if (!failed_) {
            failed_ = true;
            error_ = ParseError{std::string(file_), cur().line, cur().column, std::string(msg)};
        }
    }
    bool expect_punct(std::string_view p) {
        if (accept_punct(p)) return true;
        fail(std::string("expected '") + std::string(p) + "'");
        return false;
    }

    NodePtr mk(NodeKind k) {
        auto n = std::make_unique<Node>(k);
        n->line = cur().line; n->column = cur().column;
        return n;
    }

    // ---- statements --------------------------------------------------------
    NodePtr parse_statement() {
        if (failed_) return nullptr;
        if (is_punct(";")) { advance(); return nullptr; }
        if (is_punct("{")) return parse_block();
        if (is_kw("var") || is_kw("let") || is_kw("const")) return parse_var_decl();
        if (is_kw("async") && peek().type == TokenType::Keyword && peek().value == "function") {
            advance();  // async
            auto fn = parse_function_decl();
            if (fn) fn->is_async = true;
            return fn;
        }
        if (is_kw("function")) return parse_function_decl();
        if (is_kw("class")) return parse_class_decl();
        if (is_kw("return")) return parse_return();
        if (is_kw("if")) return parse_if();
        if (is_kw("while")) return parse_while();
        if (is_kw("with")) return parse_with();
        if (is_kw("do")) return parse_do_while();
        if (is_kw("for")) return parse_for();
        if (is_kw("switch")) return parse_switch();
        if (is_kw("throw")) return parse_throw();
        if (is_kw("try")) return parse_try();
        if (is_kw("break") || is_kw("continue")) {
            auto n = mk(is_kw("break") ? NodeKind::Break : NodeKind::Continue);
            uint32_t kw_line = cur().line; advance();
            // optional label (must be on the same line — no ASI in between)
            if (cur().type == TokenType::Identifier && cur().line == kw_line) n->str = advance().value;
            accept_punct(";");
            return n;
        }
        if (is_kw("import"))   return parse_passthrough_to_semi(NodeKind::ImportDeclaration);
        if (is_kw("export")) {
            advance();
            // `export` followed by a declaration: parse the declaration.
            auto n = mk(NodeKind::ExportDeclaration);
            if (!is_punct(";") && !at_end()) {
                auto inner = parse_statement();
                if (failed_) return nullptr;
                if (inner) n->children.push_back(std::move(inner));
            } else {
                accept_punct(";");
            }
            return n;
        }
        // labeled statement: `label: stmt`
        if (cur().type == TokenType::Identifier && peek().type == TokenType::Punctuator && peek().value == ":") {
            auto n = mk(NodeKind::Labeled);
            n->str = advance().value;  // label
            advance();                 // ':'
            auto inner = parse_statement(); if (failed_) return nullptr;
            if (inner) n->children.push_back(std::move(inner));
            return n;
        }
        // expression statement
        auto expr = parse_expression();
        if (failed_) return nullptr;
        accept_punct(";");
        auto stmt = std::make_unique<Node>(NodeKind::ExpressionStatement);
        if (expr) { stmt->line = expr->line; stmt->column = expr->column; stmt->children.push_back(std::move(expr)); }
        return stmt;
    }

    NodePtr parse_passthrough_to_semi(NodeKind kind) {
        auto n = mk(kind);
        advance();
        while (!at_end() && !is_punct(";")) advance();
        accept_punct(";");
        return n;
    }

    NodePtr parse_block() {
        auto n = mk(NodeKind::Block);
        expect_punct("{");
        while (!failed_ && !at_end() && !is_punct("}")) {
            auto s = parse_statement();
            if (failed_) return nullptr;
            if (s) n->children.push_back(std::move(s));
        }
        expect_punct("}");
        return failed_ ? nullptr : std::move(n);
    }

    // ---- binding patterns (destructuring) ------------------------------------
    // A binding target: identifier, nested array/object pattern, optionally with
    // a default. `a` -> Identifier; `a = d` -> Assignment(Identifier, default);
    // `[..]`/`{..}` -> nested pattern (also wrapped in Assignment when defaulted).
    NodePtr parse_binding_element() {
        NodePtr target;
        if (is_punct("[")) target = parse_array_pattern();
        else if (is_punct("{")) target = parse_object_pattern();
        else {
            if (cur().type != TokenType::Identifier && cur().type != TokenType::Keyword) {
                fail("expected binding name"); return nullptr;
            }
            target = mk(NodeKind::Identifier);
            target->str = advance().value;
        }
        if (failed_) return nullptr;
        if (accept_punct("=")) {
            auto def = parse_assignment(); if (failed_) return nullptr;
            auto asn = mk(NodeKind::Assignment); asn->str = "=";
            asn->children.push_back(std::move(target));
            if (def) asn->children.push_back(std::move(def));
            return asn;
        }
        return target;
    }

    // Array pattern: ArrayLiteral node. Elements are binding elements; a hole is
    // an UndefinedLiteral; a `...rest` is a Spread wrapping its target.
    NodePtr parse_array_pattern() {
        auto n = mk(NodeKind::ArrayLiteral);
        expect_punct("[");
        while (!is_punct("]") && !at_end()) {
            if (is_punct(",")) { advance(); n->children.push_back(mk(NodeKind::UndefinedLiteral)); continue; }
            if (accept_punct("...")) {
                auto t = parse_binding_element(); if (failed_) return nullptr;
                auto sp = mk(NodeKind::Spread); if (t) sp->children.push_back(std::move(t));
                n->children.push_back(std::move(sp));
                break;  // rest must be last
            }
            auto e = parse_binding_element(); if (failed_) return nullptr;
            if (e) n->children.push_back(std::move(e));
            if (!accept_punct(",")) break;
        }
        if (!expect_punct("]")) return nullptr;
        return n;
    }

    // Object pattern: ObjectLiteral node. Each property is a Member (str=key,
    // child[0]=target, optional trailing computed-key expr); `...rest` is a
    // Spread wrapping an Identifier.
    NodePtr parse_object_pattern() {
        auto n = mk(NodeKind::ObjectLiteral);
        expect_punct("{");
        while (!is_punct("}") && !at_end()) {
            if (accept_punct("...")) {
                if (cur().type != TokenType::Identifier && cur().type != TokenType::Keyword) {
                    fail("expected rest binding name"); return nullptr;
                }
                auto id = mk(NodeKind::Identifier); id->str = advance().value;
                auto sp = mk(NodeKind::Spread); sp->children.push_back(std::move(id));
                n->children.push_back(std::move(sp));
                break;
            }
            bool computed = false;
            NodePtr key_expr;
            std::string key;
            if (accept_punct("[")) {
                computed = true;
                key_expr = parse_assignment(); if (failed_) return nullptr;
                if (!expect_punct("]")) return nullptr;
            } else if (cur().type == TokenType::StringLiteral || cur().type == TokenType::NumberLiteral ||
                       cur().type == TokenType::Identifier || cur().type == TokenType::Keyword) {
                key = advance().value;
            } else { fail("expected property name in pattern"); return nullptr; }

            auto prop = mk(NodeKind::Member);
            prop->str = key;
            if (computed) prop->flags |= node_flags::Computed;

            NodePtr target;
            if (accept_punct(":")) {
                target = parse_binding_element(); if (failed_) return nullptr;
            } else {
                auto id = mk(NodeKind::Identifier); id->str = key;
                if (accept_punct("=")) {
                    auto def = parse_assignment(); if (failed_) return nullptr;
                    auto asn = mk(NodeKind::Assignment); asn->str = "=";
                    asn->children.push_back(std::move(id));
                    if (def) asn->children.push_back(std::move(def));
                    target = std::move(asn);
                } else target = std::move(id);
            }
            prop->children.insert(prop->children.begin(), std::move(target));  // child[0] = target
            if (computed && key_expr) prop->children.push_back(std::move(key_expr));
            n->children.push_back(std::move(prop));
            if (!accept_punct(",")) break;
        }
        if (!expect_punct("}")) return nullptr;
        return n;
    }

    NodePtr parse_var_decl() {
        auto n = mk(NodeKind::VariableDeclaration);
        n->str = advance().value; // var/let/const
        do {
            // Destructuring declarator: wrap as Assignment(pattern, init).
            if (is_punct("[") || is_punct("{")) {
                NodePtr pat = is_punct("[") ? parse_array_pattern() : parse_object_pattern();
                if (failed_) return nullptr;
                NodePtr init;
                if (accept_punct("=")) { init = parse_assignment(); if (failed_) return nullptr; }
                if (!init) init = mk(NodeKind::UndefinedLiteral);
                auto decl = mk(NodeKind::Assignment); decl->str = "=";
                decl->children.push_back(std::move(pat));
                decl->children.push_back(std::move(init));
                n->children.push_back(std::move(decl));
                continue;
            }
            if (cur().type != TokenType::Identifier && cur().type != TokenType::Keyword) {
                fail("expected variable name"); return nullptr;
            }
            auto id = mk(NodeKind::Identifier);
            id->str = advance().value;
            if (accept_punct("=")) {
                auto init = parse_assignment();
                if (failed_) return nullptr;
                if (init) id->children.push_back(std::move(init));
            }
            n->children.push_back(std::move(id));
        } while (accept_punct(","));
        accept_punct(";");
        return n;
    }

    // A token usable as a binding name: an Identifier, or a contextual keyword
    // (let/of/async/await/yield/static/get/set/undefined) which is not reserved.
    bool is_binding_name() const {
        if (cur().type == TokenType::Identifier) return true;
        if (cur().type == TokenType::Keyword) {
            const std::string& v = cur().value;
            return v == "let" || v == "of" || v == "async" || v == "await" ||
                   v == "yield" || v == "static" || v == "get" || v == "set" || v == "undefined";
        }
        return false;
    }

    NodePtr parse_function_decl() {
        auto n = mk(NodeKind::FunctionDeclaration);
        advance(); // function
        if (accept_punct("*")) n->flags |= node_flags::Generator;
        if (is_binding_name()) n->str = advance().value;
        if (!parse_param_list(*n)) return nullptr;
        auto body = parse_block();
        if (failed_) return nullptr;
        n->children.push_back(std::move(body));
        return n;
    }

    // Parameters become children[0..n-2] of `fn` (the body is appended last).
    // Each is an Identifier, an Assignment (default), an Array/Object pattern, or
    // a Spread (rest parameter) wrapping its target.
    bool parse_param_list(Node& fn) {
        if (!expect_punct("(")) return false;
        while (!is_punct(")") && !at_end()) {
            bool rest = accept_punct("...");
            auto p = parse_binding_element();
            if (failed_) return false;
            if (rest) {
                auto sp = mk(NodeKind::Spread);
                if (p) sp->children.push_back(std::move(p));
                p = std::move(sp);
            }
            if (p) fn.children.push_back(std::move(p));
            if (rest || !accept_punct(",")) break;  // rest must be the last parameter
        }
        return expect_punct(")");
    }

    // ClassDeclaration node layout:
    //   str          = class name (empty for an anonymous class expression)
    //   children[0]  = heritage expr (superclass), or UndefinedLiteral if none
    //   children[1..]= member nodes (NodeKind::Member; see parse_class_member)
    NodePtr parse_class_decl() {
        auto n = mk(NodeKind::ClassDeclaration);
        advance(); // class
        if (cur().type == TokenType::Identifier) n->str = advance().value;
        NodePtr heritage;
        if (accept_kw("extends")) { heritage = parse_unary(); if (failed_) return nullptr; }
        if (!heritage) heritage = mk(NodeKind::UndefinedLiteral);
        n->children.push_back(std::move(heritage));
        if (!expect_punct("{")) return nullptr;
        while (!is_punct("}") && !at_end()) {
            if (accept_punct(";")) continue;       // stray semicolons between members
            auto m = parse_class_member();
            if (failed_) return nullptr;
            if (m) n->children.push_back(std::move(m));
        }
        if (!expect_punct("}")) return nullptr;
        return n;
    }

    // A class element. Produces a Member node:
    //   str     = member key
    //   flags   = node_flags::Class{Static,Getter,Setter,Field,Computed}
    //   is_async= async method
    //   method  -> children[0] = FunctionDeclaration (params... + body block)
    //   field   -> children[0] = initializer expr (absent => undefined)
    //   computed-> children.back() = key expression (when Computed flag set)
    NodePtr parse_class_member() {
        auto m = mk(NodeKind::Member);
        // Leading modifiers. Each word ('static','async','get','set') is only a
        // modifier when it is NOT itself the member name — i.e. the next token is
        // not '(' (method), '=' (field), ';' or '}'.
        auto is_modifier_here = [&](std::string_view word) {
            if (!(cur().value == word &&
                  (cur().type == TokenType::Identifier || cur().type == TokenType::Keyword)))
                return false;
            const Token& nx = peek();
            if (nx.type == TokenType::Punctuator &&
                (nx.value == "(" || nx.value == "=" || nx.value == ";" || nx.value == "}"))
                return false;
            return true;
        };
        if (is_modifier_here("static")) { advance(); m->flags |= node_flags::ClassStatic; }
        if (is_modifier_here("async"))  { advance(); m->is_async = true; }
        bool is_gen = accept_punct("*");  // generator method
        if (is_modifier_here("get"))    { advance(); m->flags |= node_flags::ClassGetter; }
        else if (is_modifier_here("set")) { advance(); m->flags |= node_flags::ClassSetter; }

        // Member key: identifier/keyword/string/number, or computed [expr].
        if (accept_punct("[")) {
            m->flags |= node_flags::Computed;
            auto key = parse_assignment(); if (failed_) return nullptr;
            if (!expect_punct("]")) return nullptr;
            if (key) m->children.push_back(std::move(key));  // key expr trails the value
        } else if (cur().type == TokenType::StringLiteral || cur().type == TokenType::NumberLiteral ||
                   cur().type == TokenType::Identifier || cur().type == TokenType::Keyword) {
            m->str = advance().value;
        } else {
            fail("expected class member name"); return nullptr;
        }

        if (is_punct("(")) {
            // Method (or constructor / getter / setter): build a FunctionDeclaration.
            auto fn = mk(NodeKind::FunctionDeclaration);
            fn->str = m->str;
            fn->is_async = m->is_async;
            if (is_gen) fn->flags |= node_flags::Generator;
            if (!parse_param_list(*fn)) return nullptr;
            auto body = parse_block(); if (failed_) return nullptr;
            if (body) fn->children.push_back(std::move(body));
            // value goes first; any computed-key expr was appended after.
            m->children.insert(m->children.begin(), std::move(fn));
        } else {
            // Field declaration: optional initializer.
            m->flags |= node_flags::ClassField;
            NodePtr init;
            if (accept_punct("=")) { init = parse_assignment(); if (failed_) return nullptr; }
            if (!init) init = mk(NodeKind::UndefinedLiteral);
            m->children.insert(m->children.begin(), std::move(init));
            accept_punct(";");
        }
        return m;
    }

    NodePtr parse_return() {
        auto n = mk(NodeKind::Return);
        advance();
        if (!is_punct(";") && !is_punct("}") && !at_end()) {
            auto e = parse_expression();
            if (failed_) return nullptr;
            if (e) n->children.push_back(std::move(e));
        }
        accept_punct(";");
        return n;
    }

    NodePtr parse_throw() {
        auto n = mk(NodeKind::Throw);
        advance();
        auto e = parse_expression();
        if (failed_) return nullptr;
        if (e) n->children.push_back(std::move(e));
        accept_punct(";");
        return n;
    }

    NodePtr parse_if() {
        auto n = mk(NodeKind::If);
        advance();
        if (!expect_punct("(")) return nullptr;
        auto cond = parse_expression(); if (failed_) return nullptr;
        if (!expect_punct(")")) return nullptr;
        auto then = parse_statement(); if (failed_) return nullptr;
        n->children.push_back(std::move(cond));
        if (then) n->children.push_back(std::move(then));
        if (accept_kw("else")) {
            auto els = parse_statement(); if (failed_) return nullptr;
            if (els) n->children.push_back(std::move(els));
        }
        return n;
    }

    NodePtr parse_with() {
        auto n = mk(NodeKind::With);
        advance();  // with
        if (!expect_punct("(")) return nullptr;
        auto obj = parse_expression(); if (failed_) return nullptr;
        if (!expect_punct(")")) return nullptr;
        auto body = parse_statement(); if (failed_) return nullptr;
        n->children.push_back(std::move(obj));
        if (body) n->children.push_back(std::move(body));
        return n;
    }

    NodePtr parse_while() {
        auto n = mk(NodeKind::While);
        advance();
        if (!expect_punct("(")) return nullptr;
        auto cond = parse_expression(); if (failed_) return nullptr;
        if (!expect_punct(")")) return nullptr;
        auto body = parse_statement(); if (failed_) return nullptr;
        n->children.push_back(std::move(cond));
        if (body) n->children.push_back(std::move(body));
        return n;
    }

    NodePtr parse_do_while() {
        auto n = mk(NodeKind::DoWhile);
        advance();
        auto body = parse_statement(); if (failed_) return nullptr;
        if (!accept_kw("while")) { fail("expected 'while'"); return nullptr; }
        if (!expect_punct("(")) return nullptr;
        auto cond = parse_expression(); if (failed_) return nullptr;
        if (!expect_punct(")")) return nullptr;
        accept_punct(";");
        if (!body) body = mk(NodeKind::Block);
        n->children.push_back(std::move(body));
        n->children.push_back(std::move(cond));
        return n;
    }

    // For a for-loop: n->str is "of"/"in" for those forms (children = [binding,
    // iterable, body]); empty for C-style (children = [init, cond, update, body]
    // with absent parts represented by UndefinedLiteral placeholders).
    // switch (disc) { case e: stmts ... default: stmts ... }
    // Switch node: children[0]=discriminant; children[1..]=clauses. Each clause is
    // a Block whose str is "case" (children[0]=test, rest=stmts) or "default".
    NodePtr parse_switch() {
        auto n = mk(NodeKind::Switch);
        advance();  // switch
        if (!expect_punct("(")) return nullptr;
        auto disc = parse_expression(); if (failed_) return nullptr;
        if (!expect_punct(")")) return nullptr;
        n->children.push_back(disc ? std::move(disc) : mk(NodeKind::UndefinedLiteral));
        if (!expect_punct("{")) return nullptr;
        while (!is_punct("}") && !at_end()) {
            auto clause = mk(NodeKind::Block);
            if (accept_kw("case")) {
                clause->str = "case";
                auto test = parse_expression(); if (failed_) return nullptr;
                if (!expect_punct(":")) return nullptr;
                clause->children.push_back(test ? std::move(test) : mk(NodeKind::UndefinedLiteral));
            } else if (accept_kw("default")) {
                clause->str = "default";
                if (!expect_punct(":")) return nullptr;
            } else { fail("expected 'case' or 'default' in switch"); return nullptr; }
            while (!is_kw("case") && !is_kw("default") && !is_punct("}") && !at_end()) {
                auto s = parse_statement(); if (failed_) return nullptr;
                if (s) clause->children.push_back(std::move(s));
            }
            n->children.push_back(std::move(clause));
        }
        if (!expect_punct("}")) return nullptr;
        return n;
    }

    NodePtr parse_for() {
        auto n = mk(NodeKind::For);
        advance();
        accept_kw("await");  // `for await (x of y)` — async iteration (driven like for-of)
        if (!expect_punct("(")) return nullptr;

        // ---- init / binding (no trailing-semicolon consumption) ----
        // Disable the `in` operator while parsing the header so `for (x in y)` is
        // a for-in, not `(x in y)` (the ECMAScript NoIn production).
        bool saved_no_in = no_in_; no_in_ = true;
        struct RestoreNoIn { bool& f; bool v; ~RestoreNoIn() { f = v; } } restore_no_in{no_in_, saved_no_in};
        NodePtr init;
        if (is_punct(";")) {
            init = std::make_unique<Node>(NodeKind::UndefinedLiteral);
        } else if (is_kw("var") || is_kw("let") || is_kw("const")) {
            init = std::make_unique<Node>(NodeKind::VariableDeclaration);
            init->str = advance().value;  // var/let/const
            do {
                if (is_punct("[") || is_punct("{")) {  // destructuring binding
                    auto pat = is_punct("[") ? parse_array_pattern() : parse_object_pattern();
                    if (failed_) return nullptr;
                    if (accept_punct("=")) {  // C-style: pattern WITH initializer
                        auto v = parse_assignment(); if (failed_) return nullptr;
                        auto decl = std::make_unique<Node>(NodeKind::Assignment); decl->str = "=";
                        decl->children.push_back(std::move(pat));
                        decl->children.push_back(v ? std::move(v) : std::make_unique<Node>(NodeKind::UndefinedLiteral));
                        init->children.push_back(std::move(decl));
                    } else {
                        init->children.push_back(std::move(pat));  // for-of/in bare pattern
                        break;
                    }
                    continue;
                }
                if (!is_binding_name()) { fail("expected variable name"); return nullptr; }
                auto id = std::make_unique<Node>(NodeKind::Identifier);
                id->str = advance().value;
                if (accept_punct("=")) { auto v = parse_assignment(); if (failed_) return nullptr; if (v) id->children.push_back(std::move(v)); }
                init->children.push_back(std::move(id));
            } while (!is_kw("of") && !is_kw("in") && accept_punct(","));
        } else {
            init = parse_expression(); if (failed_) return nullptr;
        }
        no_in_ = saved_no_in;  // header binding parsed; `in` is an operator again

        // ---- for-of / for-in ----
        if (is_kw("of") || is_kw("in")) {
            n->str = cur().value;  // "of" or "in"
            advance();
            // for-in RHS is an Expression (allows the comma operator); for-of is
            // an AssignmentExpression.
            auto rhs = (n->str == "in") ? parse_expression() : parse_assignment();
            if (failed_) return nullptr;
            if (!expect_punct(")")) return nullptr;
            auto body = parse_statement(); if (failed_) return nullptr;
            n->children.push_back(std::move(init));
            n->children.push_back(rhs ? std::move(rhs) : std::make_unique<Node>(NodeKind::UndefinedLiteral));
            n->children.push_back(body ? std::move(body) : std::make_unique<Node>(NodeKind::Block));
            return n;
        }

        // ---- C-style ----
        if (!expect_punct(";")) return nullptr;
        NodePtr cond = is_punct(";") ? std::make_unique<Node>(NodeKind::UndefinedLiteral)
                                     : parse_expression();
        if (failed_) return nullptr;
        if (!expect_punct(";")) return nullptr;
        NodePtr update = is_punct(")") ? std::make_unique<Node>(NodeKind::UndefinedLiteral)
                                       : parse_expression();
        if (failed_) return nullptr;
        if (!expect_punct(")")) return nullptr;
        auto body = parse_statement(); if (failed_) return nullptr;
        n->children.push_back(std::move(init));
        n->children.push_back(std::move(cond));
        n->children.push_back(std::move(update));
        n->children.push_back(body ? std::move(body) : std::make_unique<Node>(NodeKind::Block));
        return n;
    }

    NodePtr parse_try() {
        auto n = mk(NodeKind::Try);
        advance();
        // n->str records which clauses are present: contains 'c' and/or 'f'.
        auto block = parse_block(); if (failed_) return nullptr; if (block) n->children.push_back(std::move(block));
        if (accept_kw("catch")) {
            std::string catch_name;
            if (accept_punct("(")) {
                if (cur().type == TokenType::Identifier) catch_name = advance().value;
                expect_punct(")");
            }
            auto cb = parse_block(); if (failed_) return nullptr;
            if (cb) { cb->str = catch_name; n->children.push_back(std::move(cb)); n->str += "c"; }
        }
        if (accept_kw("finally")) {
            auto fb = parse_block(); if (failed_) return nullptr;
            if (fb) { n->children.push_back(std::move(fb)); n->str += "f"; }
        }
        return n;
    }

    // ---- expressions -------------------------------------------------------
    NodePtr parse_expression() {
        auto first = parse_assignment();
        if (failed_) return nullptr;
        if (!is_punct(",")) return first;

        auto sequence = std::make_unique<Node>(NodeKind::Sequence);
        if (first) sequence->children.push_back(std::move(first));
        while (is_punct(",")) {
            advance();
            auto next = parse_assignment();
            if (failed_) return nullptr;
            if (next) sequence->children.push_back(std::move(next));
        }
        return sequence;
    }

    NodePtr parse_assignment() {
        // Arrow function detection: `ident =>` or `( ... ) =>`
        if (auto arrow = try_parse_arrow()) return arrow;

        auto lhs = parse_conditional();
        if (failed_) return nullptr;
        static const std::array<const char*, 16> assign_ops = {
            "=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=", "^=",
            "&&=", "||=", "?\?="
        };
        for (const char* op : assign_ops) {
            if (is_punct(op)) {
                auto n = std::make_unique<Node>(NodeKind::Assignment);
                n->str = advance().value;
                auto rhs = parse_assignment();
                if (failed_) return nullptr;
                n->children.push_back(std::move(lhs));
                if (rhs) n->children.push_back(std::move(rhs));
                return n;
            }
        }
        return lhs;
    }

    NodePtr try_parse_arrow() {
        size_t save = idx_;
        // optional leading `async` (but not `async function`)
        bool is_async = false;
        if (is_kw("async") && !(peek().type == TokenType::Keyword && peek().value == "function")) {
            is_async = true;
            advance();
        }
        // single-identifier arrow
        if (cur().type == TokenType::Identifier && peek().type == TokenType::Punctuator &&
            peek().value == "=>") {
            auto n = mk(NodeKind::ArrowFunction);
            n->is_async = is_async;
            auto p = std::make_unique<Node>(NodeKind::Identifier);
            p->str = advance().value;
            n->children.push_back(std::move(p));
            advance(); // =>
            finish_arrow_body(*n);
            return failed_ ? nullptr : std::move(n);
        }
        // parenthesized arrow: scan to matching ) then check for =>
        if (is_punct("(")) {
            int depth = 0; size_t i = idx_;
            for (; i < toks_.size(); ++i) {
                if (toks_[i].type == TokenType::Punctuator && toks_[i].value == "(") depth++;
                else if (toks_[i].type == TokenType::Punctuator && toks_[i].value == ")") { depth--; if (depth == 0) { ++i; break; } }
                else if (toks_[i].type == TokenType::EndOfFile) break;
            }
            if (i < toks_.size() && toks_[i].type == TokenType::Punctuator && toks_[i].value == "=>") {
                auto n = mk(NodeKind::ArrowFunction);
                n->is_async = is_async;
                if (!parse_param_list(*n)) { idx_ = save; failed_ = false; error_ = {}; return nullptr; }
                expect_punct("=>"); // consume =>
                finish_arrow_body(*n);
                return failed_ ? nullptr : std::move(n);
            }
        }
        idx_ = save;  // not an arrow → restore (undo tentative `async`)
        return nullptr;
    }

    void finish_arrow_body(Node& n) {
        if (is_punct("{")) {
            auto body = parse_block();
            if (!failed_ && body) n.children.push_back(std::move(body));
        } else {
            auto body = parse_assignment();
            if (!failed_ && body) n.children.push_back(std::move(body));
        }
    }

    NodePtr parse_conditional() {
        auto cond = parse_binary(0);
        if (failed_) return nullptr;
        if (accept_punct("?")) {
            auto n = std::make_unique<Node>(NodeKind::Conditional);
            auto then = parse_assignment(); if (failed_) return nullptr;
            if (!expect_punct(":")) return nullptr;
            auto els = parse_assignment(); if (failed_) return nullptr;
            n->children.push_back(std::move(cond));
            if (then) n->children.push_back(std::move(then));
            if (els) n->children.push_back(std::move(els));
            return n;
        }
        return cond;
    }

    static int binary_prec(std::string_view op) {
        if (op == "||" || op == "??") return 1;
        if (op == "&&") return 2;
        if (op == "|") return 3;
        if (op == "^") return 4;
        if (op == "&") return 5;
        if (op == "==" || op == "!=" || op == "===" || op == "!==") return 6;
        if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "instanceof" || op == "in") return 7;
        if (op == "<<" || op == ">>" || op == ">>>") return 8;
        if (op == "+" || op == "-") return 9;
        if (op == "*" || op == "/" || op == "%") return 10;
        if (op == "**") return 11;
        return -1;
    }

    NodePtr parse_binary(int min_prec) {
        auto lhs = parse_unary();
        if (failed_) return nullptr;
        while (true) {
            std::string op;
            if (cur().type == TokenType::Punctuator) op = cur().value;
            else if (is_kw("instanceof")) op = cur().value;
            else if (is_kw("in") && !no_in_) op = cur().value;  // NoIn: for-in header
            int prec = op.empty() ? -1 : binary_prec(op);
            if (prec < min_prec || prec < 0) break;
            advance();
            auto rhs = parse_binary(prec + 1);
            if (failed_) return nullptr;
            bool logical = (op == "&&" || op == "||" || op == "??");
            auto n = std::make_unique<Node>(logical ? NodeKind::Logical : NodeKind::Binary);
            n->str = op;
            n->children.push_back(std::move(lhs));
            if (rhs) n->children.push_back(std::move(rhs));
            lhs = std::move(n);
        }
        return lhs;
    }

    NodePtr parse_unary() {
        if (is_punct("!") || is_punct("-") || is_punct("+") || is_punct("~") ||
            is_punct("++") || is_punct("--") || is_kw("typeof") || is_kw("void") ||
            is_kw("delete")) {
            auto n = std::make_unique<Node>(NodeKind::Unary);
            n->str = advance().value;
            auto operand = parse_unary();
            if (failed_) return nullptr;
            if (operand) n->children.push_back(std::move(operand));
            return n;
        }
        if (is_kw("await")) { auto n = std::make_unique<Node>(NodeKind::Await); advance(); auto o = parse_unary(); if (failed_) return nullptr; if (o) n->children.push_back(std::move(o)); return n; }
        if (is_kw("yield")) { auto n = std::make_unique<Node>(NodeKind::Yield); advance(); if (accept_punct("*")) n->str = "*"; if (!is_punct(")") && !is_punct(";") && !is_punct("}") && !is_punct(",") && !is_punct("]")) { auto o = parse_assignment(); if (failed_) return nullptr; if (o) n->children.push_back(std::move(o)); } return n; }
        if (is_kw("new")) {
            advance();
            // The constructor is a MemberExpression (`.`/`[]`) but does NOT
            // swallow a following call list — that becomes the `new` arguments.
            auto ctor = parse_member_chain(parse_primary(), /*allow_call*/false);
            if (failed_) return nullptr;
            auto n = std::make_unique<Node>(NodeKind::New);
            if (is_punct("(")) {
                auto call = std::make_unique<Node>(NodeKind::Call);
                if (ctor) call->children.push_back(std::move(ctor));
                if (!parse_arguments(*call)) return nullptr;
                n->children.push_back(std::move(call));
            } else if (ctor) {
                n->children.push_back(std::move(ctor));  // `new X` without parens
            }
            // Member/call accesses after `new X(...)` apply to the new instance.
            return parse_member_chain(std::move(n), /*allow_call*/true);
        }
        return parse_postfix();
    }

    NodePtr parse_postfix() {
        auto e = parse_member_chain(parse_primary());
        if (failed_) return nullptr;
        if (is_punct("++") || is_punct("--")) {
            auto n = std::make_unique<Node>(NodeKind::Unary);
            n->str = "post" + advance().value;
            if (e) n->children.push_back(std::move(e));
            return n;
        }
        return e;
    }

    NodePtr parse_member_chain(NodePtr base, bool allow_call = true) {
        if (failed_) return nullptr;
        bool saw_optional = false;
        while (true) {
            if (!allow_call && is_punct("(")) break;  // `new` callee: stop before args
            // Optional chaining: ?.name / ?.[expr] / ?.(args)
            if (is_punct("?.")) {
                saw_optional = true;
                advance();
                if (is_punct("(")) {  // optional call
                    auto call = std::make_unique<Node>(NodeKind::Call);
                    call->flags |= node_flags::Optional;
                    if (base) call->children.push_back(std::move(base));
                    if (!parse_arguments(*call)) return nullptr;
                    base = std::move(call);
                    continue;
                }
                if (is_punct("[")) {  // optional computed member
                    advance();
                    auto m = std::make_unique<Node>(NodeKind::Member);
                    m->str = "[]"; m->flags |= node_flags::Optional;
                    auto idx = parse_expression(); if (failed_) return nullptr;
                    if (!expect_punct("]")) return nullptr;
                    if (base) m->children.push_back(std::move(base));
                    if (idx) m->children.push_back(std::move(idx));
                    base = std::move(m);
                    continue;
                }
                auto m = std::make_unique<Node>(NodeKind::Member);  // optional property
                m->flags |= node_flags::Optional;
                m->str = (cur().type == TokenType::Identifier || cur().type == TokenType::Keyword)
                             ? advance().value : "";
                if (m->str.empty()) { fail("expected property name after '?.'"); return nullptr; }
                if (base) m->children.push_back(std::move(base));
                base = std::move(m);
                continue;
            }
            if (accept_punct(".")) {
                auto m = std::make_unique<Node>(NodeKind::Member);
                m->str = (cur().type == TokenType::Identifier || cur().type == TokenType::Keyword)
                             ? advance().value : "";
                if (m->str.empty()) { fail("expected property name after '.'"); return nullptr; }
                if (base) m->children.push_back(std::move(base));
                base = std::move(m);
            } else if (accept_punct("[")) {
                auto m = std::make_unique<Node>(NodeKind::Member);
                m->str = "[]";
                auto idx = parse_expression(); if (failed_) return nullptr;
                if (!expect_punct("]")) return nullptr;
                if (base) m->children.push_back(std::move(base));
                if (idx) m->children.push_back(std::move(idx));
                base = std::move(m);
            } else if (is_punct("(")) {
                auto call = std::make_unique<Node>(NodeKind::Call);
                if (base) call->children.push_back(std::move(base));
                if (!parse_arguments(*call)) return nullptr;
                base = std::move(call);
            } else if (cur().type == TokenType::TemplateLiteral) {
                std::string raw = advance().value;
                base = build_tagged_template(std::move(base), raw);
                if (failed_) return nullptr;
            } else {
                break;
            }
        }
        if (saw_optional) {
            auto chain = std::make_unique<Node>(NodeKind::OptionalChain);
            if (base) chain->children.push_back(std::move(base));
            return chain;
        }
        return base;
    }

    bool parse_arguments(Node& call) {
        if (!expect_punct("(")) return false;
        while (!is_punct(")") && !at_end()) {
            bool spread = accept_punct("...");
            auto arg = parse_assignment();
            if (failed_) return false;
            if (spread) {
                auto sp = std::make_unique<Node>(NodeKind::Spread);
                if (arg) sp->children.push_back(std::move(arg));
                arg = std::move(sp);
            }
            if (arg) call.children.push_back(std::move(arg));
            if (!accept_punct(",")) break;
        }
        return expect_punct(")");
    }

    NodePtr parse_primary() {
        if (failed_) return nullptr;
        const Token& t = cur();
        switch (t.type) {
            case TokenType::NumberLiteral: { auto n = mk(NodeKind::NumberLiteral); n->str = advance().value; return n; }
            case TokenType::StringLiteral: { auto n = mk(NodeKind::StringLiteral); n->str = advance().value; return n; }
            case TokenType::TemplateLiteral: { std::string raw = advance().value; return build_template_expr(raw); }
            case TokenType::RegexLiteral: {
                // Lower /pat/flags to RegExp("pat", "flags"); value is pat\0flags.
                std::string raw = advance().value;
                size_t z = raw.find('\0');
                std::string pat = raw.substr(0, z);
                std::string flags = z == std::string::npos ? std::string() : raw.substr(z + 1);
                auto call = mk(NodeKind::Call);
                auto id = mk(NodeKind::Identifier); id->str = "RegExp";
                call->children.push_back(std::move(id));
                auto ps = mk(NodeKind::StringLiteral); ps->str = pat;   call->children.push_back(std::move(ps));
                auto fs = mk(NodeKind::StringLiteral); fs->str = flags; call->children.push_back(std::move(fs));
                return call;
            }
            case TokenType::Identifier: { auto n = mk(NodeKind::Identifier); n->str = advance().value; return n; }
            case TokenType::Keyword: {
                if (t.value == "true" || t.value == "false") { auto n = mk(NodeKind::BoolLiteral); n->str = advance().value; return n; }
                if (t.value == "null") { auto n = mk(NodeKind::NullLiteral); advance(); return n; }
                if (t.value == "undefined") { auto n = mk(NodeKind::UndefinedLiteral); advance(); return n; }
                if (t.value == "this" || t.value == "super") { auto n = mk(NodeKind::Identifier); n->str = advance().value; return n; }
                if (t.value == "function") return parse_function_decl();
                if (t.value == "async") {
                    advance();
                    if (is_kw("function")) { auto fn = parse_function_decl(); if (fn) fn->is_async = true; return fn; }
                    return parse_primary();  // 'async' as identifier (arrows handled earlier)
                }
                if (t.value == "class") return parse_class_decl();
                // treat other keywords used as identifiers leniently
                auto n = mk(NodeKind::Identifier); n->str = advance().value; return n;
            }
            case TokenType::Punctuator:
                if (t.value == "(") {
                    advance();
                    auto e = parse_expression();
                    if (failed_) return nullptr;
                    if (!expect_punct(")")) return nullptr;
                    return e;
                }
                if (t.value == "[") return parse_array_literal();
                if (t.value == "{") return parse_object_literal();
                fail(std::string("unexpected token '") + t.value + "'");
                return nullptr;
            default:
                fail("unexpected end of input");
                return nullptr;
        }
    }

    NodePtr parse_array_literal() {
        auto n = mk(NodeKind::ArrayLiteral);
        expect_punct("[");
        while (!is_punct("]") && !at_end()) {
            if (accept_punct(",")) continue; // elision
            bool spread = accept_punct("...");
            auto e = parse_assignment(); if (failed_) return nullptr;
            if (spread) {
                auto sp = std::make_unique<Node>(NodeKind::Spread);
                if (e) sp->children.push_back(std::move(e));
                e = std::move(sp);
            }
            if (e) n->children.push_back(std::move(e));
            if (!accept_punct(",")) break;
        }
        if (!expect_punct("]")) return nullptr;
        return n;
    }

    // Each entry is a Member node: str = key, child[0] = value expression.
    NodePtr parse_object_literal() {
        auto n = mk(NodeKind::ObjectLiteral);
        expect_punct("{");
        while (!is_punct("}") && !at_end()) {
            if (accept_punct("...")) {
                auto e = parse_assignment(); if (failed_) return nullptr;
                auto spread = std::make_unique<Node>(NodeKind::Spread);
                if (e) spread->children.push_back(std::move(e));
                n->children.push_back(std::move(spread));
                if (!accept_punct(",")) break;
                continue;
            }
            std::string key;
            bool computed = false;
            bool is_async_method = false;
            if (cur().value == "async" &&
                (cur().type == TokenType::Identifier ||
                 cur().type == TokenType::Keyword)) {
                const Token& next = peek();
                const bool same_line = cur().line == next.line;
                const bool followed_by_key =
                    next.type == TokenType::Identifier ||
                    next.type == TokenType::Keyword ||
                    next.type == TokenType::StringLiteral ||
                    next.type == TokenType::NumberLiteral ||
                    (next.type == TokenType::Punctuator &&
                     (next.value == "[" || next.value == "*"));
                if (same_line && followed_by_key) {
                    advance();
                    is_async_method = true;
                }
            }
            // get/set accessor: `get name(){}` / `set name(v){}` — but NOT when
            // `get`/`set` is itself the key (`{get: 1}`, `{get(){}}`).
            uint8_t accessor_flag = 0;
            if ((cur().value == "get" || cur().value == "set") &&
                (cur().type == TokenType::Identifier || cur().type == TokenType::Keyword)) {
                const Token& nx = peek();
                bool next_is_key = nx.type == TokenType::Identifier || nx.type == TokenType::Keyword ||
                                   nx.type == TokenType::StringLiteral || nx.type == TokenType::NumberLiteral ||
                                   (nx.type == TokenType::Punctuator && nx.value == "[");
                if (next_is_key) accessor_flag = (cur().value == "get") ? node_flags::ClassGetter : node_flags::ClassSetter;
                if (accessor_flag) advance();
            }
            bool is_gen_method = accept_punct("*");  // generator method shorthand
            NodePtr key_expr;
            if (accept_punct("[")) {
                computed = true;
                key_expr = parse_assignment(); if (failed_) return nullptr;
                expect_punct("]");
            } else if (cur().type == TokenType::StringLiteral || cur().type == TokenType::NumberLiteral ||
                       cur().type == TokenType::Identifier || cur().type == TokenType::Keyword) {
                key = advance().value;
            } else { fail("expected property key"); return nullptr; }

            auto prop = std::make_unique<Node>(NodeKind::Member);
            prop->str = key;
            if (computed) prop->flags |= node_flags::Computed;
            prop->flags |= accessor_flag;

            if (accessor_flag) {
                // accessor: parse `(params) { body }` as a FunctionDeclaration
                auto fn = std::make_unique<Node>(NodeKind::FunctionDeclaration);
                if (!parse_param_list(*fn)) return nullptr;
                auto body = parse_block(); if (failed_) return nullptr; if (body) fn->children.push_back(std::move(body));
                prop->children.push_back(std::move(fn));
            } else if (accept_punct(":")) {
                auto v = parse_assignment(); if (failed_) return nullptr;
                if (v) prop->children.push_back(std::move(v));
            } else if (is_punct("(")) {
                // method shorthand: key() { ... }
                auto fn = std::make_unique<Node>(NodeKind::FunctionDeclaration);
                fn->is_async = is_async_method;
                if (is_gen_method) fn->flags |= node_flags::Generator;
                if (!parse_param_list(*fn)) return nullptr;
                auto body = parse_block(); if (failed_) return nullptr; if (body) fn->children.push_back(std::move(body));
                prop->children.push_back(std::move(fn));
            } else {
                if (is_async_method) {
                    fail("expected '(' after async method name");
                    return nullptr;
                }
                // shorthand { key } => { key: key }
                auto id = std::make_unique<Node>(NodeKind::Identifier);
                id->str = key;
                prop->children.push_back(std::move(id));
            }
            if (computed && key_expr) prop->children.push_back(std::move(key_expr));  // trailing key expr
            n->children.push_back(std::move(prop));
            if (!accept_punct(",")) break;
        }
        if (!expect_punct("}")) return nullptr;
        return n;
    }

    // ---- template literals (interpolation) -----------------------------------
    NodePtr make_plus(NodePtr a, NodePtr b) {
        auto n = std::make_unique<Node>(NodeKind::Binary); n->str = "+";
        n->children.push_back(std::move(a));
        n->children.push_back(std::move(b));
        return n;
    }
    static std::string cook_escape(const std::string& r, size_t& i) {
        ++i;  // backslash
        if (i >= r.size()) return "\\";
        char c = r[i++];
        switch (c) {
            case 'n': return "\n"; case 't': return "\t"; case 'r': return "\r";
            case 'b': return "\b"; case 'f': return "\f"; case 'v': return "\v";
            case '0': return std::string(1, '\0');
            case '`': return "`"; case '$': return "$"; case '\\': return "\\";
            case '\'': return "'"; case '"': return "\"";
            case 'x': { int v = 0; if (i + 1 < r.size()) { v = (hex_digit(r[i]) << 4) | hex_digit(r[i + 1]); i += 2; } std::string s; utf8_append(s, static_cast<uint32_t>(v)); return s; }
            case 'u': { uint32_t cp = 0; if (i < r.size() && r[i] == '{') { ++i; while (i < r.size() && r[i] != '}') cp = cp * 16 + hex_digit(r[i++]); if (i < r.size()) ++i; } else { for (int k = 0; k < 4 && i < r.size(); ++k) cp = cp * 16 + hex_digit(r[i++]); } std::string s; utf8_append(s, cp); return s; }
            default: return std::string(1, c);
        }
    }
    static size_t skip_string(const std::string& r, size_t i, char q) {
        while (i < r.size()) { if (r[i] == '\\') { i += 2; continue; } if (r[i] == q) return i + 1; ++i; } return i;
    }
    static size_t skip_interp(const std::string& r, size_t i);  // fwd (mutual)
    static size_t skip_template(const std::string& r, size_t i) {  // after opening backtick
        while (i < r.size()) {
            if (r[i] == '\\') { i += 2; continue; }
            if (r[i] == '`') return i + 1;
            if (r[i] == '$' && i + 1 < r.size() && r[i + 1] == '{') { i = skip_interp(r, i + 2); continue; }
            ++i;
        }
        return i;
    }
    // Parses an interpolation source as an expression (reuses the full parser, so
    // nested templates recurse). Returns "" on empty/failed interpolation.
    NodePtr parse_interp(const std::string& inner) {
        malibu::js::parser::Parser sub;
        auto r = sub.parse(inner, "template");
        if (r.program && !r.program->children.empty()) {
            Node* st = r.program->children[0].get();
            if (st->kind == NodeKind::ExpressionStatement && !st->children.empty())
                return std::move(st->children[0]);
        }
        auto sn = std::make_unique<Node>(NodeKind::StringLiteral); sn->str = ""; return sn;
    }
    // Lowers a raw template body into a string-concatenation expression.
    NodePtr build_template_expr(const std::string& raw) {
        NodePtr expr;
        std::string lit;
        auto flush = [&]() {
            auto sn = std::make_unique<Node>(NodeKind::StringLiteral);
            sn->str = lit; lit.clear();
            expr = expr ? make_plus(std::move(expr), std::move(sn)) : std::move(sn);
        };
        for (size_t i = 0; i < raw.size();) {
            if (raw[i] == '\\') { lit += cook_escape(raw, i); continue; }
            if (raw[i] == '$' && i + 1 < raw.size() && raw[i + 1] == '{') {
                flush();  // emit preceding literal (string-coerces the chain)
                size_t end = skip_interp(raw, i + 2);  // index just after the matching '}'
                size_t inner_len = (end > i + 3) ? (end - 1) - (i + 2) : 0;
                std::string inner = raw.substr(i + 2, inner_len);
                expr = make_plus(std::move(expr), parse_interp(inner));
                i = end;
                continue;
            }
            lit += raw[i++];
        }
        flush();
        return expr ? std::move(expr) : [&]{ auto sn = std::make_unique<Node>(NodeKind::StringLiteral); sn->str = ""; return sn; }();
    }

    static std::string cook_template_segment(const std::string& raw) {
        std::string cooked;
        for (size_t i = 0; i < raw.size();) {
            if (raw[i] == '\\') cooked += cook_escape(raw, i);
            else cooked += raw[i++];
        }
        return cooked;
    }

    NodePtr build_tagged_template(NodePtr tag, const std::string& raw) {
        auto n = std::make_unique<Node>(NodeKind::TaggedTemplate);
        if (tag) n->children.push_back(std::move(tag));

        size_t segment_start = 0;
        for (size_t i = 0; i < raw.size();) {
            if (raw[i] == '\\') {
                i = std::min(i + 2, raw.size());
                continue;
            }
            if (raw[i] != '$' || i + 1 >= raw.size() || raw[i + 1] != '{') {
                ++i;
                continue;
            }

            std::string segment = raw.substr(segment_start, i - segment_start);
            n->template_raw.push_back(segment);
            n->template_cooked.push_back(cook_template_segment(segment));

            size_t end = skip_interp(raw, i + 2);
            if (end <= i + 2 || end > raw.size()) {
                fail("unterminated tagged template interpolation");
                return nullptr;
            }
            size_t inner_len = (end - 1) - (i + 2);
            n->children.push_back(parse_interp(raw.substr(i + 2, inner_len)));
            i = end;
            segment_start = end;
        }

        std::string segment = raw.substr(segment_start);
        n->template_raw.push_back(segment);
        n->template_cooked.push_back(cook_template_segment(segment));
        return n;
    }

    std::vector<Token> toks_;
    std::string_view   file_;
    size_t             idx_    = 0;
    bool               failed_ = false;
    bool               no_in_  = false;  // disables the `in` operator (for-in header)
    ParseError         error_;
};

// Skips a regex literal in `r` starting just after the opening '/'.
static size_t skip_regex_lit(const std::string& r, size_t i) {
    bool in_class = false;
    while (i < r.size()) {
        char c = r[i];
        if (c == '\\') { i += 2; continue; }
        if (c == '[') in_class = true;
        else if (c == ']') in_class = false;
        else if (c == '/' && !in_class) { ++i; break; }
        ++i;
    }
    while (i < r.size() && std::isalnum(static_cast<unsigned char>(r[i]))) ++i;
    return i;
}

// Out-of-line (mutually recursive with skip_template).
size_t TokenParser::skip_interp(const std::string& r, size_t i) {
    int depth = 1;
    char last = '(';
    while (i < r.size()) {
        char c = r[i];
        if (c == '\\') { i += 2; last = 'x'; continue; }
        if (c == '`') { i = skip_template(r, i + 1); last = '`'; continue; }
        if (c == '"' || c == '\'') { i = skip_string(r, i + 1, c); last = c; continue; }
        if (c == '/' && i + 1 < r.size() && r[i + 1] != '/' && r[i + 1] != '*' && regex_after(last)) { i = skip_regex_lit(r, i + 1); last = '/'; continue; }
        if (c == '{') { ++depth; ++i; last = '{'; continue; }
        if (c == '}') { --depth; ++i; if (depth == 0) return i; last = '}'; continue; }
        if (!std::isspace(static_cast<unsigned char>(c))) last = c;
        ++i;
    }
    return i;
}

} // namespace

Parser::Result Parser::parse(std::string_view source, std::string_view filename) {
    Result result;

    Lexer lexer(source, filename);
    std::vector<Token> tokens;
    ParseError lex_err;
    if (!lexer.tokenize(tokens, lex_err)) {
        result.errors.push_back(std::move(lex_err));
        return result; // no AST emitted on lexical error
    }

    TokenParser parser(std::move(tokens), filename);
    return parser.run();
}

} // namespace malibu::js::parser
