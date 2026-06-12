// css/parser/css_parser.cpp
// Hand-written CSS Syntax Level 3 tokenizer + parser.

#include "malibu/css/parser/css_parser.h"
#include "malibu/css/selector/selector.h"
#include "malibu/diagnostics/diagnostic_log.h"

#include <algorithm>
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

bool balanced_value(std::u16string_view value) {
    std::vector<char16_t> stack;
    char16_t quote = 0;
    bool escaped = false;
    for (char16_t c : value) {
        if (quote != 0) {
            if (escaped) {
                escaped = false;
            } else if (c == u'\\') {
                escaped = true;
            } else if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == u'"' || c == u'\'') {
            quote = c;
        } else if (c == u'(' || c == u'[' || c == u'{') {
            stack.push_back(c);
        } else if (c == u')' || c == u']' || c == u'}') {
            if (stack.empty()) return false;
            const char16_t expected =
                c == u')' ? u'(' : c == u']' ? u'[' : u'{';
            if (stack.back() != expected) return false;
            stack.pop_back();
        } else if (c == u';') {
            return false;
        }
    }
    return quote == 0 && stack.empty();
}

bool is_supported_property(std::u16string_view property) {
    static constexpr std::u16string_view properties[] = {
        u"align-items", u"aspect-ratio", u"background",
        u"background-color", u"background-image", u"border",
        u"border-bottom", u"border-bottom-color", u"border-bottom-width",
        u"border-color", u"border-left", u"border-left-color",
        u"border-left-width", u"border-radius", u"border-right",
        u"border-right-color", u"border-right-width", u"border-style",
        u"border-top", u"border-top-color", u"border-top-width",
        u"border-width", u"bottom", u"box-shadow", u"box-sizing",
        u"clear", u"color", u"column-gap", u"content", u"display",
        u"fill", u"flex", u"flex-basis", u"flex-direction",
        u"flex-grow", u"flex-shrink", u"flex-wrap", u"float", u"font",
        u"font-family", u"font-size", u"font-style", u"font-weight",
        u"gap", u"grid-gap", u"grid-template-columns", u"height",
        u"justify-content", u"left", u"line-height", u"list-style",
        u"list-style-type", u"margin", u"margin-bottom", u"margin-left",
        u"margin-right", u"margin-top", u"max-height", u"max-width",
        u"min-height", u"min-width", u"object-fit", u"opacity",
        u"overflow", u"padding", u"padding-bottom", u"padding-left",
        u"padding-right", u"padding-top", u"position", u"right",
        u"row-gap", u"text-align", u"text-decoration",
        u"text-decoration-line", u"text-transform", u"top", u"transform",
        u"vertical-align", u"visibility", u"white-space", u"width",
        u"z-index",
    };
    return std::find(std::begin(properties), std::end(properties), property) !=
           std::end(properties);
}

bool one_of(std::u16string_view value,
            std::initializer_list<std::u16string_view> values) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool supported_keyword_value(std::u16string_view property,
                             std::u16string_view value) {
    if (one_of(value, {u"inherit", u"initial", u"unset", u"revert",
                       u"revert-layer"}))
        return true;
    if (property == u"display")
        return one_of(value, {u"block", u"inline", u"inline-block", u"flex",
                              u"inline-flex", u"grid", u"inline-grid",
                              u"list-item", u"table", u"inline-table",
                              u"table-row", u"table-cell", u"table-row-group",
                              u"table-header-group", u"table-footer-group",
                              u"none"});
    if (property == u"position")
        return one_of(value, {u"static", u"relative", u"absolute", u"fixed",
                              u"sticky"});
    if (property == u"float")
        return one_of(value, {u"none", u"left", u"right"});
    if (property == u"clear")
        return one_of(value, {u"none", u"left", u"right", u"both"});
    if (property == u"box-sizing")
        return one_of(value, {u"content-box", u"border-box"});
    if (property == u"visibility")
        return one_of(value, {u"visible", u"hidden", u"collapse"});
    if (property == u"overflow")
        return one_of(value, {u"visible", u"hidden", u"scroll", u"auto"});
    if (property == u"object-fit")
        return one_of(value, {u"fill", u"contain", u"cover", u"none",
                              u"scale-down"});
    if (property == u"flex-direction")
        return one_of(value, {u"row", u"row-reverse", u"column",
                              u"column-reverse"});
    if (property == u"flex-wrap")
        return one_of(value, {u"nowrap", u"wrap", u"wrap-reverse"});
    if (property == u"align-items")
        return one_of(value, {u"stretch", u"flex-start", u"flex-end",
                              u"center", u"baseline"});
    if (property == u"justify-content")
        return one_of(value, {u"flex-start", u"flex-end", u"center",
                              u"space-between", u"space-around",
                              u"space-evenly"});
    if (property == u"white-space")
        return one_of(value, {u"normal", u"nowrap", u"pre", u"pre-wrap"});
    if (property == u"text-transform")
        return one_of(value, {u"none", u"uppercase", u"lowercase",
                              u"capitalize"});
    if (property == u"vertical-align")
        return one_of(value, {u"baseline", u"top", u"middle", u"bottom",
                              u"sub", u"super"});
    if (property == u"font-style")
        return one_of(value, {u"normal", u"italic", u"oblique"});
    return true;
}

bool supported_pseudo_classes(const ComplexSelector& selector) {
    static constexpr std::u16string_view pseudos[] = {
        u"active", u"any-link", u"checked", u"disabled", u"empty",
        u"enabled", u"first-child", u"focus", u"focus-within", u"hover",
        u"is", u"last-child", u"link", u"matches", u"not", u"nth-child",
        u"nth-last-child", u"nth-last-of-type", u"nth-of-type",
        u"only-child", u"root", u"where",
    };
    for (const auto& step : selector.steps) {
        for (const auto& pseudo : step.second.pseudos) {
            if (std::find(std::begin(pseudos), std::end(pseudos),
                          pseudo.name) == std::end(pseudos))
                return false;
        }
    }
    return true;
}

size_t find_top_level(std::u16string_view text,
                      std::u16string_view needle) {
    int depth = 0;
    for (size_t i = 0; i + needle.size() <= text.size(); ++i) {
        if (text[i] == u'(') ++depth;
        else if (text[i] == u')') --depth;
        if (depth == 0 && text.substr(i, needle.size()) == needle) return i;
    }
    return std::u16string_view::npos;
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

bool supports_property_value(std::u16string_view raw_property,
                             std::u16string_view raw_value) {
    const std::u16string property = lower(trim(raw_property));
    const std::u16string value = lower(trim(raw_value));
    if (property.empty() || value.empty() || !balanced_value(value))
        return false;
    if (property.rfind(u"--", 0) == 0) return property.size() > 2;
    return is_supported_property(property) &&
           supported_keyword_value(property, value);
}

bool supports_selector(std::u16string_view raw_selector) {
    const std::u16string selector = trim(raw_selector);
    if (selector.empty()) return false;
    const ComplexSelector parsed = parse_selector(selector);
    return parsed.valid && supported_pseudo_classes(parsed);
}

bool supports_condition(std::u16string_view raw_condition) {
    std::u16string condition = lower(trim(raw_condition));
    if (condition.empty()) return false;
    if (condition.rfind(u"not ", 0) == 0)
        return !supports_condition(condition.substr(4));

    size_t op = find_top_level(condition, u" and ");
    if (op != std::u16string::npos)
        return supports_condition(condition.substr(0, op)) &&
               supports_condition(condition.substr(op + 5));
    op = find_top_level(condition, u" or ");
    if (op != std::u16string::npos)
        return supports_condition(condition.substr(0, op)) ||
               supports_condition(condition.substr(op + 4));

    if (condition.rfind(u"selector(", 0) == 0 &&
        condition.back() == u')')
        return supports_selector(
            condition.substr(9, condition.size() - 10));
    if (condition.front() == u'(' && condition.back() == u')') {
        const std::u16string inner =
            trim(condition.substr(1, condition.size() - 2));
        const size_t colon = inner.find(u':');
        if (colon != std::u16string::npos)
            return supports_property_value(inner.substr(0, colon),
                                           inner.substr(colon + 1));
        return supports_condition(inner);
    }
    return false;
}

} // namespace malibu::css
