// css/selector/selector.cpp
// CSS selector parsing, specificity, and DOM matching.

#include "malibu/css/selector/selector.h"
#include "malibu/dom/document.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace malibu::css {
namespace {

std::u16string lower(std::u16string s) {
    for (auto& c : s) if (c >= u'A' && c <= u'Z') c = c - u'A' + u'a';
    return s;
}
bool is_name_char(char16_t c) {
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') ||
           (c >= u'0' && c <= u'9') || c == u'-' || c == u'_' || c > 127;
}

// Parses one compound selector starting at i (i advanced past it).
CompoundSelector parse_compound(std::u16string_view s, size_t& i, Specificity& spec, bool& ok) {
    CompoundSelector comp;
    ok = false;
    auto read_name = [&]() {
        std::u16string n;
        while (i < s.size() && is_name_char(s[i])) n.push_back(s[i++]);
        return n;
    };
    while (i < s.size()) {
        char16_t c = s[i];
        if (c == u' ' || c == u'\t' || c == u'\n' || c == u'>' || c == u'+' || c == u'~' || c == u',') break;
        if (c == u'*') { comp.universal = true; ++i; ok = true; }
        else if (c == u'.') { ++i; comp.classes.push_back(read_name()); spec.b++; ok = true; }
        else if (c == u'#') { ++i; comp.id = read_name(); spec.a++; ok = true; }
        else if (c == u'[') {
            ++i;
            AttrSelector attr;
            std::u16string name;
            while (i < s.size() && is_name_char(s[i])) name.push_back(s[i++]);
            attr.name = lower(name);
            if (i < s.size() && s[i] != u']') {
                char16_t op = s[i];
                if (op == u'~') { attr.op = AttrSelector::Op::Includes; i += 2; }
                else if (op == u'^') { attr.op = AttrSelector::Op::Prefix; i += 2; }
                else if (op == u'$') { attr.op = AttrSelector::Op::Suffix; i += 2; }
                else if (op == u'*') { attr.op = AttrSelector::Op::Substring; i += 2; }
                else if (op == u'=') { attr.op = AttrSelector::Op::Equals; i += 1; }
                std::u16string val;
                if (i < s.size() && (s[i] == u'"' || s[i] == u'\'')) {
                    char16_t q = s[i++];
                    while (i < s.size() && s[i] != q) val.push_back(s[i++]);
                    if (i < s.size()) ++i;
                } else {
                    while (i < s.size() && s[i] != u']') val.push_back(s[i++]);
                }
                attr.value = val;
            }
            if (i < s.size() && s[i] == u']') ++i;
            comp.attrs.push_back(std::move(attr));
            spec.b++; ok = true;
        }
        else if (c == u':') {
            ++i;
            if (i < s.size() && s[i] == u':') { ++i; read_name(); spec.c++; ok = true; continue; }  // pseudo-element
            PseudoClass pc;
            pc.name = lower(read_name());
            if (i < s.size() && s[i] == u'(') {
                ++i;
                std::u16string arg;
                int depth = 1;
                while (i < s.size() && depth > 0) {
                    if (s[i] == u'(') ++depth;
                    else if (s[i] == u')') { if (--depth == 0) break; }
                    arg.push_back(s[i++]);
                }
                if (i < s.size()) ++i;  // ')'
                pc.arg = arg;
            }
            comp.pseudos.push_back(std::move(pc));
            spec.b++; ok = true;
        }
        else if (is_name_char(c)) { comp.tag = lower(read_name()); spec.c++; ok = true; }
        else { ++i; }  // skip unknown char
    }
    return comp;
}

}  // namespace

ComplexSelector parse_selector(std::u16string_view text) {
    ComplexSelector sel;
    size_t i = 0;
    Combinator pending = Combinator::Descendant;
    bool first = true;
    auto skip_ws = [&]() { while (i < text.size() && (text[i] == u' ' || text[i] == u'\t' || text[i] == u'\n')) ++i; };

    while (i < text.size()) {
        skip_ws();
        if (i >= text.size()) break;
        if (text[i] == u'>') { pending = Combinator::Child; ++i; skip_ws(); }
        else if (text[i] == u'+') { pending = Combinator::AdjacentSibling; ++i; skip_ws(); }
        else if (text[i] == u'~') { pending = Combinator::GeneralSibling; ++i; skip_ws(); }

        bool ok = false;
        CompoundSelector comp = parse_compound(text, i, sel.specificity, ok);
        if (ok) {
            sel.steps.emplace_back(first ? Combinator::Descendant : pending, std::move(comp));
            first = false;
            pending = Combinator::Descendant;
        } else if (i < text.size()) {
            ++i;  // avoid infinite loop on garbage
        }
    }
    sel.valid = !sel.steps.empty();
    return sel;
}

// ---- matching ----
namespace {
using malibu::dom::Document;
using malibu::dom::NodeCore;

std::vector<std::u16string> split_ws(const std::u16string& s) {
    std::vector<std::u16string> out;
    std::u16string cur;
    for (char16_t c : s) {
        if (c == u' ' || c == u'\t' || c == u'\n' || c == u'\r') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

const std::u16string* attr_value(const NodeCore* c, const std::u16string& name) {
    for (auto& [k, v] : c->attributes) if (k == name) return &v;
    return nullptr;
}

int index_among_element_siblings(Document& doc, malibu::NodeHandle node, bool from_end) {
    const NodeCore* c = doc.core(node);
    if (!c || c->parent.is_null()) return 1;
    const NodeCore* p = doc.core(c->parent);
    if (!p) return 1;
    std::vector<malibu::NodeHandle> elems;
    for (auto ch : p->children) {
        const NodeCore* cc = doc.core(ch);
        if (cc && cc->node_type == malibu::dom::kElementNode) elems.push_back(ch);
    }
    if (from_end) std::reverse(elems.begin(), elems.end());
    for (size_t k = 0; k < elems.size(); ++k) if (elems[k] == node) return static_cast<int>(k) + 1;
    return 1;
}

bool matches_compound(Document& doc, malibu::NodeHandle node, const CompoundSelector& comp);   // fwd

// Matches an An+B / odd / even / integer nth formula against a 1-based index.
bool nth_matches(const std::u16string& arg, int idx) {
    std::u16string s; for (char16_t c : arg) if (c != u' ') s.push_back(c == u'A' || c == u'N' ? c + 32 : c);
    if (s == u"odd")  return idx % 2 == 1;
    if (s == u"even") return idx % 2 == 0;
    int a = 0, b = 0;
    size_t np = s.find(u'n');
    if (np == std::u16string::npos) {                       // plain integer
        try { return idx == std::stoi(std::string(s.begin(), s.end())); } catch (...) { return false; }
    }
    std::u16string as = s.substr(0, np);                    // coefficient of n
    if (as.empty() || as == u"+") a = 1; else if (as == u"-") a = -1;
    else { try { a = std::stoi(std::string(as.begin(), as.end())); } catch (...) { a = 0; } }
    std::u16string bs = s.substr(np + 1);                   // +b / -b
    if (!bs.empty()) { try { b = std::stoi(std::string(bs.begin(), bs.end())); } catch (...) { b = 0; } }
    if (a == 0) return idx == b;
    return ((idx - b) % a == 0) && ((idx - b) / a >= 0);
}

// Parses a selector-list argument (for :not/:is/:where) into compound selectors.
std::vector<CompoundSelector> parse_arg_compounds(const std::u16string& arg) {
    std::vector<CompoundSelector> out;
    size_t start = 0;
    while (start <= arg.size()) {
        size_t comma = arg.find(u',', start);
        std::u16string part = arg.substr(start, comma == std::u16string::npos ? std::u16string::npos : comma - start);
        start = comma == std::u16string::npos ? arg.size() + 1 : comma + 1;
        size_t i = 0; Specificity sp; bool ok = false;
        CompoundSelector c = parse_compound(part, i, sp, ok);
        if (ok) out.push_back(std::move(c));
    }
    return out;
}

bool matches_pseudo(Document& doc, malibu::NodeHandle node, const PseudoClass& pc) {
    if (pc.name == u"first-child") return index_among_element_siblings(doc, node, false) == 1;
    if (pc.name == u"last-child")  return index_among_element_siblings(doc, node, true) == 1;
    if (pc.name == u"only-child")  return index_among_element_siblings(doc, node, false) == 1 && index_among_element_siblings(doc, node, true) == 1;
    if (pc.name == u"root") return node == doc.root() || doc.core(node) == nullptr ? false :
                                   (doc.core(node)->parent == doc.root());
    if (pc.name == u"nth-child" || pc.name == u"nth-of-type")
        return nth_matches(pc.arg, index_among_element_siblings(doc, node, false));
    if (pc.name == u"nth-last-child" || pc.name == u"nth-last-of-type")
        return nth_matches(pc.arg, index_among_element_siblings(doc, node, true));
    if (pc.name == u"not") {
        for (auto& comp : parse_arg_compounds(pc.arg)) if (matches_compound(doc, node, comp)) return false;
        return true;
    }
    if (pc.name == u"is" || pc.name == u"where" || pc.name == u"matches") {
        for (auto& comp : parse_arg_compounds(pc.arg)) if (matches_compound(doc, node, comp)) return true;
        return false;
    }
    if (pc.name == u"checked")  { const NodeCore* c = doc.core(node); return c && attr_value(c, u"checked") != nullptr; }
    if (pc.name == u"disabled") { const NodeCore* c = doc.core(node); return c && attr_value(c, u"disabled") != nullptr; }
    if (pc.name == u"enabled")  { const NodeCore* c = doc.core(node); return c && attr_value(c, u"disabled") == nullptr &&
                                         (c->tag_name == u"input" || c->tag_name == u"button" || c->tag_name == u"select" || c->tag_name == u"textarea"); }
    if (pc.name == u"empty") { const NodeCore* c = doc.core(node); return c && c->children.empty(); }
    // Dynamic interaction pseudo-classes, driven by NodeCore flags set on input.
    if (pc.name == u"hover")  { const NodeCore* c = doc.core(node); return c && c->hovered; }
    if (pc.name == u"focus")  { const NodeCore* c = doc.core(node); return c && c->focused; }
    if (pc.name == u"active") { const NodeCore* c = doc.core(node); return c && c->active; }
    if (pc.name == u"focus-within") {
        for (malibu::NodeHandle n = node; doc.core(n); n = doc.core(n)->parent)
            if (doc.core(n)->focused) return true;
        return false;
    }
    if (pc.name == u"link" || pc.name == u"any-link") {
        const NodeCore* c = doc.core(node);
        return c && c->tag_name == u"a" && attr_value(c, u"href") != nullptr;
    }
    return false;
}

bool matches_compound(Document& doc, malibu::NodeHandle node, const CompoundSelector& comp) {
    const NodeCore* c = doc.core(node);
    if (!c || c->node_type != malibu::dom::kElementNode) return false;
    if (!comp.universal && !comp.tag.empty() && c->tag_name != comp.tag) return false;
    if (!comp.id.empty()) {
        const std::u16string* id = attr_value(c, u"id");
        if (!id || *id != comp.id) return false;
    }
    if (!comp.classes.empty()) {
        const std::u16string* cl = attr_value(c, u"class");
        std::vector<std::u16string> have = cl ? split_ws(*cl) : std::vector<std::u16string>{};
        for (auto& want : comp.classes)
            if (std::find(have.begin(), have.end(), want) == have.end()) return false;
    }
    for (auto& attr : comp.attrs) {
        const std::u16string* v = attr_value(c, attr.name);
        if (!v) return false;
        switch (attr.op) {
            case AttrSelector::Op::Exists: break;
            case AttrSelector::Op::Equals: if (*v != attr.value) return false; break;
            case AttrSelector::Op::Includes: {
                auto parts = split_ws(*v);
                if (std::find(parts.begin(), parts.end(), attr.value) == parts.end()) return false;
                break;
            }
            case AttrSelector::Op::Prefix: if (v->rfind(attr.value, 0) != 0) return false; break;
            case AttrSelector::Op::Suffix:
                if (v->size() < attr.value.size() || v->compare(v->size() - attr.value.size(), attr.value.size(), attr.value) != 0) return false;
                break;
            case AttrSelector::Op::Substring: if (v->find(attr.value) == std::u16string::npos) return false; break;
        }
    }
    for (auto& pc : comp.pseudos) if (!matches_pseudo(doc, node, pc)) return false;
    return true;
}

malibu::NodeHandle previous_element_sibling(Document& doc, malibu::NodeHandle node) {
    const NodeCore* c = doc.core(node);
    if (!c || c->parent.is_null()) return malibu::NodeHandle::null_handle();
    const NodeCore* p = doc.core(c->parent);
    if (!p) return malibu::NodeHandle::null_handle();
    malibu::NodeHandle prev = malibu::NodeHandle::null_handle();
    for (auto ch : p->children) {
        if (ch == node) return prev;
        const NodeCore* cc = doc.core(ch);
        if (cc && cc->node_type == malibu::dom::kElementNode) prev = ch;
    }
    return malibu::NodeHandle::null_handle();
}

}  // namespace

bool matches(Document& doc, malibu::NodeHandle node, const ComplexSelector& sel) {
    if (!sel.valid || sel.steps.empty()) return false;
    int si = static_cast<int>(sel.steps.size()) - 1;
    if (!matches_compound(doc, node, sel.steps[si].second)) return false;

    malibu::NodeHandle cur = node;
    --si;
    while (si >= 0) {
        Combinator comb = sel.steps[si + 1].first;
        const CompoundSelector& target = sel.steps[si].second;
        const NodeCore* c = doc.core(cur);
        if (!c) return false;

        if (comb == Combinator::Child) {
            malibu::NodeHandle p = c->parent;
            if (p.is_null() || !matches_compound(doc, p, target)) return false;
            cur = p;
        } else if (comb == Combinator::Descendant) {
            malibu::NodeHandle p = c->parent;
            bool found = false;
            while (!p.is_null()) {
                if (matches_compound(doc, p, target)) { found = true; cur = p; break; }
                const NodeCore* pc = doc.core(p);
                p = pc ? pc->parent : malibu::NodeHandle::null_handle();
            }
            if (!found) return false;
        } else if (comb == Combinator::AdjacentSibling) {
            malibu::NodeHandle prev = previous_element_sibling(doc, cur);
            if (prev.is_null() || !matches_compound(doc, prev, target)) return false;
            cur = prev;
        } else {  // GeneralSibling
            malibu::NodeHandle prev = previous_element_sibling(doc, cur);
            bool found = false;
            while (!prev.is_null()) {
                if (matches_compound(doc, prev, target)) { found = true; cur = prev; break; }
                prev = previous_element_sibling(doc, prev);
            }
            if (!found) return false;
        }
        --si;
    }
    return true;
}

} // namespace malibu::css
