// html/html_parser.cpp
// Single-pass HTML tokenizer + tree builder producing a NodeTable-backed DOM.

#include "malibu/html/html_parser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace malibu::html {
namespace {

using malibu::dom::DOMTree;
using malibu::NodeHandle;

std::string narrow(const std::u16string& s) { std::string r; for (char16_t c : s) r.push_back(static_cast<char>(c & 0xFF)); return r; }
std::u16string lower(std::u16string s) { for (auto& c : s) if (c >= u'A' && c <= u'Z') c = c - u'A' + u'a'; return s; }

bool is_void(const std::u16string& t) {
    static const std::array<const char16_t*, 14> v = {
        u"area", u"base", u"br", u"col", u"embed", u"hr", u"img", u"input",
        u"link", u"meta", u"param", u"source", u"track", u"wbr"};
    for (auto* e : v) if (t == e) return true;
    return false;
}
bool is_raw_text(const std::u16string& t) { return t == u"script" || t == u"style" || t == u"textarea" || t == u"title"; }

// Implied-end-tag rules for a few common cases (auto-closing).
bool closes_p(const std::u16string& t) {
    static const std::array<const char16_t*, 18> b = {
        u"address", u"article", u"aside", u"blockquote", u"div", u"dl", u"fieldset",
        u"footer", u"form", u"h1", u"h2", u"h3", u"h4", u"header", u"hr", u"ol", u"p", u"ul"};
    for (auto* e : b) if (t == e) return true;
    return false;
}

const std::unordered_map<std::u16string, char16_t>& named_entities() {
    static const std::unordered_map<std::u16string, char16_t> m = {
        // punctuation / typography
        {u"bull", u'•'}, {u"middot", u'·'}, {u"dagger", u'†'}, {u"Dagger", u'‡'},
        {u"lsquo", u'‘'}, {u"rsquo", u'’'}, {u"ldquo", u'“'}, {u"rdquo", u'”'},
        {u"sbquo", u'‚'}, {u"bdquo", u'„'}, {u"prime", u'′'}, {u"Prime", u'″'},
        {u"permil", u'‰'}, {u"laquo", u'«'}, {u"raquo", u'»'}, {u"iexcl", u'¡'},
        {u"iquest", u'¿'}, {u"sect", u'§'}, {u"para", u'¶'}, {u"shy", u'­'},
        {u"ensp", u' '}, {u"emsp", u' '}, {u"thinsp", u' '},
        // currency
        {u"cent", u'¢'}, {u"pound", u'£'}, {u"yen", u'¥'}, {u"euro", u'€'}, {u"curren", u'¤'},
        // math / misc symbols
        {u"times", u'×'}, {u"divide", u'÷'}, {u"deg", u'°'}, {u"plusmn", u'±'},
        {u"micro", u'µ'}, {u"sup1", u'¹'}, {u"sup2", u'²'}, {u"sup3", u'³'},
        {u"frac14", u'¼'}, {u"frac12", u'½'}, {u"frac34", u'¾'}, {u"minus", u'−'},
        {u"ne", u'≠'}, {u"le", u'≤'}, {u"ge", u'≥'}, {u"infin", u'∞'},
        {u"asymp", u'≈'}, {u"radic", u'√'}, {u"larr", u'←'}, {u"uarr", u'↑'},
        {u"rarr", u'→'}, {u"darr", u'↓'}, {u"harr", u'↔'},
        // accented Latin (Spanish/French/German/Portuguese)
        {u"Agrave", u'À'}, {u"Aacute", u'Á'}, {u"Acirc", u'Â'}, {u"Atilde", u'Ã'},
        {u"Auml", u'Ä'}, {u"Aring", u'Å'}, {u"AElig", u'Æ'}, {u"Ccedil", u'Ç'},
        {u"Egrave", u'È'}, {u"Eacute", u'É'}, {u"Ecirc", u'Ê'}, {u"Euml", u'Ë'},
        {u"Igrave", u'Ì'}, {u"Iacute", u'Í'}, {u"Icirc", u'Î'}, {u"Iuml", u'Ï'},
        {u"Ntilde", u'Ñ'}, {u"Ograve", u'Ò'}, {u"Oacute", u'Ó'}, {u"Ocirc", u'Ô'},
        {u"Otilde", u'Õ'}, {u"Ouml", u'Ö'}, {u"Oslash", u'Ø'}, {u"Ugrave", u'Ù'},
        {u"Uacute", u'Ú'}, {u"Ucirc", u'Û'}, {u"Uuml", u'Ü'}, {u"Yacute", u'Ý'},
        {u"szlig", u'ß'}, {u"agrave", u'à'}, {u"aacute", u'á'}, {u"acirc", u'â'},
        {u"atilde", u'ã'}, {u"auml", u'ä'}, {u"aring", u'å'}, {u"aelig", u'æ'},
        {u"ccedil", u'ç'}, {u"egrave", u'è'}, {u"eacute", u'é'}, {u"ecirc", u'ê'},
        {u"euml", u'ë'}, {u"igrave", u'ì'}, {u"iacute", u'í'}, {u"icirc", u'î'},
        {u"iuml", u'ï'}, {u"ntilde", u'ñ'}, {u"ograve", u'ò'}, {u"oacute", u'ó'},
        {u"ocirc", u'ô'}, {u"otilde", u'õ'}, {u"ouml", u'ö'}, {u"oslash", u'ø'},
        {u"ugrave", u'ù'}, {u"uacute", u'ú'}, {u"ucirc", u'û'}, {u"uuml", u'ü'},
        {u"yacute", u'ý'}, {u"yuml", u'ÿ'},
        {u"amp", u'&'}, {u"lt", u'<'}, {u"gt", u'>'}, {u"quot", u'"'}, {u"apos", u'\''},
        {u"nbsp", u' '}, {u"copy", u'©'}, {u"reg", u'®'}, {u"mdash", u'—'},
        {u"ndash", u'–'}, {u"hellip", u'…'}, {u"trade", u'™'},
    };
    return m;
}

std::u16string decode_entities(std::u16string_view s) {
    std::u16string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == u'&') {
            size_t semi = s.find(u';', i + 1);
            if (semi != std::u16string_view::npos && semi - i <= 10) {
                std::u16string ref(s.substr(i + 1, semi - i - 1));
                if (!ref.empty() && ref[0] == u'#') {
                    long code = 0;
                    try {
                        code = (ref.size() > 1 && (ref[1] == u'x' || ref[1] == u'X'))
                                   ? std::stol(narrow(ref.substr(2)), nullptr, 16)
                                   : std::stol(narrow(ref.substr(1)));
                        if (code > 0xFFFF && code <= 0x10FFFF) {  // astral → UTF-16 surrogate pair
                            code -= 0x10000;
                            out.push_back(static_cast<char16_t>(0xD800 + (code >> 10)));
                            out.push_back(static_cast<char16_t>(0xDC00 + (code & 0x3FF)));
                        } else {
                            out.push_back(static_cast<char16_t>(code));
                        }
                        i = semi + 1;
                        continue;
                    } catch (...) {}
                } else {
                    auto it = named_entities().find(ref);
                    if (it != named_entities().end()) { out.push_back(it->second); i = semi + 1; continue; }
                }
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

class Builder {
public:
    Builder(std::u16string_view src, DOMTree& tree) : src_(src), tree_(tree) {
        stack_.push_back(tree_.document().root());
    }
    // Fragment ctor: build under `context` instead of the document root.
    Builder(std::u16string_view src, DOMTree& tree, NodeHandle context) : src_(src), tree_(tree) {
        stack_.push_back(context);
    }

    ParsedDocument run() {
        while (pos_ < src_.size()) {
            if (src_[pos_] == u'<') {
                if (starts_with(u"<!--")) { skip_comment(); continue; }
                if (starts_with(u"<!")) { skip_decl(); continue; }
                if (pos_ + 1 < src_.size() && src_[pos_ + 1] == u'/') { parse_end_tag(); continue; }
                parse_start_tag();
            } else {
                parse_text();
            }
        }
        return std::move(result_);
    }

private:
    NodeHandle current() { return stack_.back(); }

    bool starts_with(std::u16string_view p) const { return src_.compare(pos_, p.size(), p) == 0; }
    char16_t peek(size_t o = 0) const { return pos_ + o < src_.size() ? src_[pos_ + o] : u'\0'; }

    void skip_comment() {
        size_t end = src_.find(u"-->", pos_ + 4);
        pos_ = end == std::u16string_view::npos ? src_.size() : end + 3;
    }
    void skip_decl() {
        size_t end = src_.find(u'>', pos_);
        pos_ = end == std::u16string_view::npos ? src_.size() : end + 1;
    }

    void parse_text() {
        size_t start = pos_;
        while (pos_ < src_.size() && src_[pos_] != u'<') ++pos_;
        std::u16string text = decode_entities(src_.substr(start, pos_ - start));
        // Keep non-empty text (including significant whitespace runs).
        if (!text.empty()) {
            NodeHandle tn = tree_.create_text_node(text);
            tree_.append_child(current(), tn);
        }
    }

    std::u16string read_tag_name() {
        size_t start = pos_;
        while (pos_ < src_.size()) {
            char16_t c = src_[pos_];
            if (c == u'>' || c == u'/' || std::isspace(static_cast<unsigned char>(c & 0xFF))) break;
            ++pos_;
        }
        return lower(std::u16string(src_.substr(start, pos_ - start)));
    }

    void skip_ws() { while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_] & 0xFF))) ++pos_; }

    void parse_start_tag() {
        ++pos_;  // '<'
        std::u16string name = read_tag_name();
        if (name.empty()) { // stray '<'
            NodeHandle tn = tree_.create_text_node(u"<");
            tree_.append_child(current(), tn);
            return;
        }

        // implied-end-tag: a new block closes an open <p>
        if (closes_p(name)) {
            for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
                const auto* c = tree_.document().core(*it);
                if (c && c->tag_name == u"p") { close_to(u"p"); break; }
                if (it + 1 == stack_.rend()) break;
            }
        }
        // Auto-close elements with optional end tags when a sibling/structural
        // tag opens (li, dt/dd, option, tr, td/th, thead/tbody/tfoot) — real
        // pages routinely omit these end tags.
        apply_implied_end(name);

        NodeHandle el = tree_.create_element(name);
        std::vector<std::pair<std::u16string, std::u16string>> attrs;
        bool self_closing = false;
        parse_attributes(attrs, self_closing);
        for (auto& [k, v] : attrs) tree_.set_attribute(el, k, v);
        tree_.append_child(current(), el);

        // External stylesheet: <link rel="stylesheet" href="...">.
        if (name == u"link") {
            std::u16string rel, href;
            for (auto& [k, v] : attrs) { if (k == u"rel") rel = v; else if (k == u"href") href = v; }
            if (!href.empty() && rel.find(u"stylesheet") != std::u16string::npos)
                result_.external_styles.push_back(href);
        }

        if (is_raw_text(name)) {
            std::u16string content = read_raw_until_end(name);
            if (!content.empty()) {
                NodeHandle tn = tree_.create_text_node(content);
                tree_.append_child(el, tn);
            }
            if (name == u"script") {
                std::u16string src;
                for (auto& [k, v] : attrs) if (k == u"src") src = v;
                if (!src.empty()) result_.script_items.push_back(ScriptItem{true, {}, src});
                else { result_.scripts.push_back(content); result_.script_items.push_back(ScriptItem{false, content, {}}); }
            } else if (name == u"style") {
                result_.stylesheets.push_back(content);
            }
            return;
        }
        if (!self_closing && !is_void(name)) stack_.push_back(el);
    }

    void parse_attributes(std::vector<std::pair<std::u16string, std::u16string>>& out, bool& self_closing) {
        while (pos_ < src_.size()) {
            skip_ws();
            if (pos_ >= src_.size()) break;
            char16_t c = src_[pos_];
            if (c == u'>') { ++pos_; return; }
            if (c == u'/') { self_closing = true; ++pos_; continue; }
            // attribute name
            size_t ns = pos_;
            while (pos_ < src_.size()) {
                char16_t ch = src_[pos_];
                if (ch == u'=' || ch == u'>' || ch == u'/' || std::isspace(static_cast<unsigned char>(ch & 0xFF))) break;
                ++pos_;
            }
            std::u16string name = lower(std::u16string(src_.substr(ns, pos_ - ns)));
            std::u16string value;
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == u'=') {
                ++pos_; skip_ws();
                if (pos_ < src_.size() && (src_[pos_] == u'"' || src_[pos_] == u'\'')) {
                    char16_t q = src_[pos_++];
                    size_t vs = pos_;
                    while (pos_ < src_.size() && src_[pos_] != q) ++pos_;
                    value = decode_entities(src_.substr(vs, pos_ - vs));
                    if (pos_ < src_.size()) ++pos_;  // closing quote
                } else {
                    size_t vs = pos_;
                    while (pos_ < src_.size()) {
                        char16_t ch = src_[pos_];
                        if (ch == u'>' || std::isspace(static_cast<unsigned char>(ch & 0xFF))) break;
                        ++pos_;
                    }
                    value = decode_entities(src_.substr(vs, pos_ - vs));
                }
            }
            if (!name.empty()) out.emplace_back(name, value);
        }
    }

    std::u16string read_raw_until_end(const std::u16string& tag) {
        std::u16string close = u"</" + tag;
        size_t start = pos_;
        size_t p = pos_;
        while (p < src_.size()) {
            if (src_[p] == u'<' && src_.compare(p, close.size(), close) == 0) break;
            ++p;
        }
        std::u16string content(src_.substr(start, p - start));
        // advance past the end tag
        size_t gt = src_.find(u'>', p);
        pos_ = gt == std::u16string_view::npos ? src_.size() : gt + 1;
        return content;
    }

    void parse_end_tag() {
        pos_ += 2;  // '</'
        std::u16string name = read_tag_name();
        size_t gt = src_.find(u'>', pos_);
        pos_ = gt == std::u16string_view::npos ? src_.size() : gt + 1;
        close_to(name);
    }

    std::u16string top_name() {
        const auto* c = tree_.document().core(current());
        return c ? c->tag_name : std::u16string();
    }

    // Closes currently-open elements that have optional end tags before a new
    // start tag (HTML5 "generate implied end tags" for the common cases).
    void apply_implied_end(const std::u16string& name) {
        std::u16string t = top_name();
        if (name == u"li" && t == u"li") close_to(u"li");
        else if ((name == u"dt" || name == u"dd") && (t == u"dt" || t == u"dd")) close_to(t);
        else if (name == u"option" && t == u"option") close_to(u"option");
        else if (name == u"optgroup" && t == u"option") close_to(u"option");
        else if ((name == u"td" || name == u"th") && (t == u"td" || t == u"th")) close_to(t);
        else if (name == u"tr") {
            if (t == u"td" || t == u"th") close_to(t);
            if (top_name() == u"tr") close_to(u"tr");
        } else if (name == u"thead" || name == u"tbody" || name == u"tfoot") {
            for (;;) {
                std::u16string x = top_name();
                if (x == u"td" || x == u"th" || x == u"tr" || x == u"thead" || x == u"tbody" || x == u"tfoot")
                    close_to(x);
                else break;
            }
        }
    }

    // Pops open elements up to and including the nearest element named `name`.
    void close_to(const std::u16string& name) {
        for (size_t i = stack_.size(); i-- > 1;) {  // never pop the document root
            const auto* c = tree_.document().core(stack_[i]);
            if (c && c->tag_name == name) {
                stack_.resize(i);
                return;
            }
        }
        // no matching open element: ignore the stray end tag
    }

    std::u16string_view     src_;
    DOMTree&                tree_;
    size_t                  pos_ = 0;
    std::vector<NodeHandle> stack_;
    ParsedDocument          result_;
};

} // namespace

ParsedDocument HTMLParser::parse(std::u16string_view html, malibu::dom::DOMTree& tree) {
    Builder b(html, tree);
    return b.run();
}

ParsedDocument HTMLParser::parse_fragment(std::u16string_view html, malibu::dom::DOMTree& tree,
                                          malibu::NodeHandle context) {
    Builder b(html, tree, context);
    return b.run();
}

} // namespace malibu::html
