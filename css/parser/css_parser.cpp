// css/parser/css_parser.cpp
// Hand-written CSS Syntax Level 3 tokenizer + parser.

#include "malibu/css/parser/css_parser.h"
#include "malibu/diagnostics/diagnostic_log.h"

#include <cctype>
#include <string>

namespace malibu::css {
namespace {

std::u16string lower(std::u16string s) {
    for (auto& c : s) if (c >= u'A' && c <= u'Z') c = c - u'A' + u'a';
    return s;
}

std::u16string trim(std::u16string_view s) {
    size_t b = 0, e = s.size();
    auto ws = [](char16_t c) { return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r' || c == u'\f'; };
    while (b < e && ws(s[b])) ++b;
    while (e > b && ws(s[e - 1])) --e;
    return std::u16string(s.substr(b, e - b));
}

class Parser {
public:
    explicit Parser(std::u16string_view src) : src_(src) {}

    StyleSheet parse() {
        StyleSheet sheet;
        skip_ws_and_comments();
        while (pos_ < src_.size()) {
            if (src_[pos_] == u'@') {
                if (try_parse_media(sheet)) { skip_ws_and_comments(); continue; }
                skip_at_rule(); skip_ws_and_comments(); continue;
            }
            Rule rule;
            if (parse_rule(rule)) sheet.rules.push_back(std::move(rule));
            skip_ws_and_comments();
        }
        return sheet;
    }

private:
    char16_t peek(size_t o = 0) const { return pos_ + o < src_.size() ? src_[pos_ + o] : u'\0'; }
    char16_t advance() {
        char16_t c = src_[pos_++];
        if (c == u'\n') { ++line_; col_ = 1; } else { ++col_; }
        return c;
    }
    SourceLocation loc() const { return {line_, col_}; }

    void skip_ws_and_comments() {
        for (;;) {
            while (pos_ < src_.size() &&
                   (src_[pos_] == u' ' || src_[pos_] == u'\t' || src_[pos_] == u'\n' ||
                    src_[pos_] == u'\r' || src_[pos_] == u'\f'))
                advance();
            if (pos_ + 1 < src_.size() && src_[pos_] == u'/' && src_[pos_ + 1] == u'*') {
                advance(); advance();
                while (pos_ < src_.size() && !(peek() == u'*' && peek(1) == u'/')) advance();
                if (pos_ < src_.size()) { advance(); advance(); }
                continue;
            }
            break;
        }
    }

    // @media/@import/@font-face/...: consume the at-rule (block or ;-terminated).
    void skip_at_rule() {
        while (pos_ < src_.size() && src_[pos_] != u'{' && src_[pos_] != u';') advance();
        if (pos_ < src_.size() && src_[pos_] == u';') { advance(); return; }
        if (pos_ < src_.size() && src_[pos_] == u'{') {
            int depth = 0;
            do {
                char16_t c = advance();
                if (c == u'{') ++depth; else if (c == u'}') --depth;
            } while (pos_ < src_.size() && depth > 0);
        }
    }

    // @media <condition> { <rules> }: parse the inner rules, tagging each with
    // the condition so the cascade can evaluate it against the viewport. Returns
    // false if this isn't an @media at-rule (caller falls back to skip_at_rule).
    bool try_parse_media(StyleSheet& sheet) {
        size_t save = pos_;
        if (src_.compare(pos_, 6, u"@media") != 0) return false;
        pos_ += 6;
        std::u16string cond;
        while (pos_ < src_.size() && src_[pos_] != u'{' && src_[pos_] != u';') cond.push_back(advance());
        if (pos_ >= src_.size() || src_[pos_] != u'{') { pos_ = save; return false; }
        advance();  // '{'
        std::u16string m = trim(cond);
        skip_ws_and_comments();
        while (pos_ < src_.size() && src_[pos_] != u'}') {
            if (src_[pos_] == u'@') { skip_at_rule(); skip_ws_and_comments(); continue; }
            Rule rule;
            if (parse_rule(rule)) { rule.media = m; sheet.rules.push_back(std::move(rule)); }
            skip_ws_and_comments();
        }
        if (pos_ < src_.size() && src_[pos_] == u'}') advance();
        return true;
    }

    bool parse_rule(Rule& out) {
        // prelude (selector list) up to '{'
        std::u16string prelude;
        while (pos_ < src_.size() && src_[pos_] != u'{' && src_[pos_] != u'}') {
            if (peek() == u'/' && peek(1) == u'*') { skip_ws_and_comments(); continue; }
            prelude.push_back(advance());
        }
        if (pos_ >= src_.size() || src_[pos_] != u'{') {
            // malformed rule with no block
            if (pos_ < src_.size() && src_[pos_] == u'}') advance();
            return false;
        }
        advance();  // '{'

        // selectors: split prelude on top-level commas
        for (auto& sel_text : split_selectors(prelude)) {
            std::u16string t = trim(sel_text);
            if (!t.empty()) out.selectors.push_back(Selector{t});
        }

        // declarations
        parse_declaration_block(out.declarations);
        return !out.selectors.empty();
    }

    std::vector<std::u16string> split_selectors(const std::u16string& s) {
        std::vector<std::u16string> parts;
        std::u16string cur;
        int depth = 0;
        for (char16_t c : s) {
            if (c == u'(' || c == u'[') ++depth;
            else if (c == u')' || c == u']') --depth;
            if (c == u',' && depth == 0) { parts.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        parts.push_back(cur);
        return parts;
    }

    void parse_declaration_block(std::vector<Declaration>& out) {
        for (;;) {
            skip_ws_and_comments();
            if (pos_ >= src_.size() || src_[pos_] == u'}') { if (pos_ < src_.size()) advance(); return; }
            parse_one_declaration(out);
        }
    }

    void parse_one_declaration(std::vector<Declaration>& out) {
        SourceLocation start = loc();
        std::u16string name;
        while (pos_ < src_.size() && src_[pos_] != u':' && src_[pos_] != u';' && src_[pos_] != u'}')
            name.push_back(advance());

        if (pos_ >= src_.size() || src_[pos_] != u':') {
            // malformed: no colon — discard to ';' or '}', log, continue
            while (pos_ < src_.size() && src_[pos_] != u';' && src_[pos_] != u'}') advance();
            if (pos_ < src_.size() && src_[pos_] == u';') advance();
            MALIBU_LOG(WARNING, "css",
                       "malformed declaration (no ':') at " + std::to_string(start.line) +
                           ":" + std::to_string(start.column));
            return;
        }
        advance();  // ':'

        std::u16string value;
        bool in_str = false; char16_t quote = 0;
        while (pos_ < src_.size()) {
            char16_t c = src_[pos_];
            if (in_str) { value.push_back(advance()); if (c == quote) in_str = false; continue; }
            if (c == u'"' || c == u'\'') { in_str = true; quote = c; value.push_back(advance()); continue; }
            if (c == u';' || c == u'}') break;
            value.push_back(advance());
        }
        if (pos_ < src_.size() && src_[pos_] == u';') advance();

        Declaration d;
        d.property = lower(trim(name));
        d.loc = start;
        std::u16string v = trim(value);
        // !important
        std::u16string lv = lower(v);
        size_t bang = lv.rfind(u"!important");
        if (bang != std::u16string::npos) {
            d.important = true;
            v = trim(v.substr(0, bang));
        }
        d.value = v;

        if (d.property.empty() || d.value.empty()) {
            MALIBU_LOG(WARNING, "css",
                       "malformed declaration at " + std::to_string(start.line) +
                           ":" + std::to_string(start.column));
            return;
        }
        out.push_back(std::move(d));
    }

    std::u16string_view src_;
    size_t   pos_ = 0;
    uint32_t line_ = 1;
    uint32_t col_ = 1;
};

}  // namespace

StyleSheet CSSParser::parse(std::u16string_view source) {
    Parser p(source);
    return p.parse();
}

} // namespace malibu::css
