// js/runtime/regex.cpp
// Backtracking regular-expression engine (see regex.h). Parses a pattern into a
// small node tree, then matches with continuation-passing backtracking.

#include "malibu/js/runtime/regex.h"

#include <functional>
#include <memory>

namespace malibu::js::runtime::regex {
namespace {

enum class RK { Char, Any, Class, Start, End, WordB, NWordB, Group, Concat, Alt, Repeat, Empty };

struct RNode {
    RK kind;
    char16_t ch = 0;                                        // Char
    bool negate = false;                                    // Class negation
    std::vector<std::pair<char16_t, char16_t>> ranges;      // Class ranges (inclusive)
    std::vector<int> specials;                              // Class \d=1 \D=2 \w=3 \W=4 \s=5 \S=6
    int group_index = -1;                                   // Group: capture index (-1 non-capturing)
    int min = 0, max = -1;                                  // Repeat: max=-1 => unbounded
    bool greedy = true;                                     // Repeat
    std::vector<std::unique_ptr<RNode>> kids;               // Concat/Alt; Group/Repeat use kids[0]
    explicit RNode(RK k) : kind(k) {}
};
using NodePtr = std::unique_ptr<RNode>;

bool is_word_char(char16_t c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool is_space_char(char16_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// ---- parser ----------------------------------------------------------------
struct Parser {
    const std::u16string& p;
    size_t i = 0;
    int group_count = 0;   // number of capturing groups assigned
    bool ok = true;

    bool at_end() const { return i >= p.size(); }
    char16_t cur() const { return i < p.size() ? p[i] : 0; }

    NodePtr parse() {
        auto n = parse_alt();
        if (!at_end()) ok = false;  // trailing ')' etc.
        return n;
    }

    NodePtr parse_alt() {
        auto first = parse_concat();
        if (cur() != '|') return first;
        auto alt = std::make_unique<RNode>(RK::Alt);
        alt->kids.push_back(std::move(first));
        while (cur() == '|') { ++i; alt->kids.push_back(parse_concat()); }
        return alt;
    }

    NodePtr parse_concat() {
        auto seq = std::make_unique<RNode>(RK::Concat);
        while (!at_end() && cur() != '|' && cur() != ')') {
            auto atom = parse_quantified();
            if (!atom) break;
            seq->kids.push_back(std::move(atom));
        }
        if (seq->kids.empty()) return std::make_unique<RNode>(RK::Empty);
        if (seq->kids.size() == 1) return std::move(seq->kids[0]);
        return seq;
    }

    NodePtr parse_quantified() {
        auto atom = parse_atom();
        if (!atom) return atom;
        char16_t c = cur();
        if (c == '*' || c == '+' || c == '?' || c == '{') {
            int lo = 0, hi = -1;
            if (c == '*') { lo = 0; hi = -1; ++i; }
            else if (c == '+') { lo = 1; hi = -1; ++i; }
            else if (c == '?') { lo = 0; hi = 1; ++i; }
            else { // {n} {n,} {n,m}
                size_t save = i; ++i;
                int n1 = 0; bool any = false;
                while (cur() >= '0' && cur() <= '9') { n1 = n1 * 10 + (cur() - '0'); ++i; any = true; }
                int n2 = n1;
                if (cur() == ',') { ++i; if (cur() == '}') n2 = -1; else { n2 = 0; while (cur() >= '0' && cur() <= '9') { n2 = n2 * 10 + (cur() - '0'); ++i; } } }
                if (cur() != '}' || !any) { i = save; return atom; }  // not a quantifier: literal '{'
                ++i; lo = n1; hi = n2;
            }
            auto rep = std::make_unique<RNode>(RK::Repeat);
            rep->min = lo; rep->max = hi;
            if (cur() == '?') { rep->greedy = false; ++i; }
            rep->kids.push_back(std::move(atom));
            return rep;
        }
        return atom;
    }

    NodePtr parse_atom() {
        char16_t c = cur();
        if (c == '(') {
            ++i;
            int gi = -1;
            if (cur() == '?' && i + 1 < p.size() && p[i + 1] == ':') { i += 2; }  // non-capturing
            else gi = ++group_count;
            auto inner = parse_alt();
            if (cur() == ')') ++i; else ok = false;
            auto g = std::make_unique<RNode>(RK::Group);
            g->group_index = gi;
            g->kids.push_back(std::move(inner));
            return g;
        }
        if (c == '[') return parse_class();
        if (c == '.') { ++i; return std::make_unique<RNode>(RK::Any); }
        if (c == '^') { ++i; return std::make_unique<RNode>(RK::Start); }
        if (c == '$') { ++i; return std::make_unique<RNode>(RK::End); }
        if (c == '\\') return parse_escape();
        if (c == ')' || c == '|' || c == 0) return nullptr;
        ++i;
        auto n = std::make_unique<RNode>(RK::Char); n->ch = c; return n;
    }

    // Escape outside a class. Predefined classes become a Class node.
    NodePtr parse_escape() {
        ++i;  // backslash
        char16_t e = cur(); ++i;
        if (e == 'b') return std::make_unique<RNode>(RK::WordB);
        if (e == 'B') return std::make_unique<RNode>(RK::NWordB);
        int special = special_code(e);
        if (special) { auto n = std::make_unique<RNode>(RK::Class); n->specials.push_back(special); return n; }
        auto n = std::make_unique<RNode>(RK::Char); n->ch = escape_char(e); return n;
    }

    NodePtr parse_class() {
        ++i;  // '['
        auto n = std::make_unique<RNode>(RK::Class);
        if (cur() == '^') { n->negate = true; ++i; }
        while (!at_end() && cur() != ']') {
            char16_t c = cur();
            if (c == '\\') {
                ++i; char16_t e = cur(); ++i;
                int sp = special_code(e);
                if (sp) { n->specials.push_back(sp); continue; }
                c = escape_char(e);
            } else ++i;
            // range a-z?
            if (cur() == '-' && i + 1 < p.size() && p[i + 1] != ']') {
                ++i; char16_t hi = cur();
                if (hi == '\\') { ++i; hi = escape_char(cur()); }
                ++i;
                n->ranges.emplace_back(c, hi);
            } else {
                n->ranges.emplace_back(c, c);
            }
        }
        if (cur() == ']') ++i; else ok = false;
        return n;
    }

    static int special_code(char16_t e) {
        switch (e) {
            case 'd': return 1; case 'D': return 2;
            case 'w': return 3; case 'W': return 4;
            case 's': return 5; case 'S': return 6;
            default:  return 0;
        }
    }
    static char16_t escape_char(char16_t e) {
        switch (e) {
            case 'n': return '\n'; case 't': return '\t'; case 'r': return '\r';
            case 'f': return '\f'; case 'v': return '\v'; case '0': return '\0';
            default:  return e;  // \. \\ \/ \( ... => the literal char
        }
    }
};

// ---- matcher ---------------------------------------------------------------
struct Matcher {
    const std::u16string& s;
    bool ignore_case;
    bool multiline;
    std::vector<std::pair<long, long>> groups;  // [0]=whole, [1..]=captures
    using Cont = std::function<bool(size_t)>;

    char16_t fold(char16_t c) const {
        if (ignore_case && c >= 'A' && c <= 'Z') return static_cast<char16_t>(c - 'A' + 'a');
        return c;
    }
    bool char_eq(char16_t a, char16_t b) const { return fold(a) == fold(b); }

    bool special_match(int sp, char16_t c) const {
        switch (sp) {
            case 1: return c >= '0' && c <= '9';
            case 2: return !(c >= '0' && c <= '9');
            case 3: return is_word_char(c);
            case 4: return !is_word_char(c);
            case 5: return is_space_char(c);
            case 6: return !is_space_char(c);
        }
        return false;
    }

    bool class_match(const RNode* n, char16_t c) const {
        bool hit = false;
        for (auto& [lo, hi] : n->ranges) {
            if (ignore_case) { if (fold(c) >= fold(lo) && fold(c) <= fold(hi)) { hit = true; break; }
                               if (c >= lo && c <= hi) { hit = true; break; } }
            else if (c >= lo && c <= hi) { hit = true; break; }
        }
        if (!hit) for (int sp : n->specials) if (special_match(sp, c)) { hit = true; break; }
        return n->negate ? !hit : hit;
    }

    bool word_boundary(size_t pos) const {
        bool before = pos > 0 && is_word_char(s[pos - 1]);
        bool after = pos < s.size() && is_word_char(s[pos]);
        return before != after;
    }

    bool match(const RNode* n, size_t pos, const Cont& k) {
        switch (n->kind) {
            case RK::Empty: return k(pos);
            case RK::Char:  return pos < s.size() && char_eq(s[pos], n->ch) && k(pos + 1);
            case RK::Any:   return pos < s.size() && s[pos] != '\n' && k(pos + 1);
            case RK::Class: return pos < s.size() && class_match(n, s[pos]) && k(pos + 1);
            case RK::Start: return ((pos == 0) || (multiline && s[pos - 1] == '\n')) && k(pos);
            case RK::End:   return ((pos == s.size()) || (multiline && s[pos] == '\n')) && k(pos);
            case RK::WordB: return word_boundary(pos) && k(pos);
            case RK::NWordB:return !word_boundary(pos) && k(pos);
            case RK::Concat: return match_seq(n, 0, pos, k);
            case RK::Alt:
                for (auto& kid : n->kids) if (match(kid.get(), pos, k)) return true;
                return false;
            case RK::Group: {
                int g = n->group_index;
                const RNode* child = n->kids[0].get();
                if (g < 0) return match(child, pos, k);
                long saved_lo = groups[g].first, saved_hi = groups[g].second;
                size_t start = pos;
                bool ok = match(child, pos, [&](size_t end) {
                    long olo = groups[g].first, ohi = groups[g].second;
                    groups[g] = {static_cast<long>(start), static_cast<long>(end)};
                    if (k(end)) return true;
                    groups[g] = {olo, ohi};
                    return false;
                });
                if (!ok) groups[g] = {saved_lo, saved_hi};
                return ok;
            }
            case RK::Repeat: return match_repeat(n->kids[0].get(), n, 0, pos, k);
        }
        return false;
    }

    bool match_seq(const RNode* n, size_t idx, size_t pos, const Cont& k) {
        if (idx == n->kids.size()) return k(pos);
        return match(n->kids[idx].get(), pos, [&](size_t p) { return match_seq(n, idx + 1, p, k); });
    }

    bool match_repeat(const RNode* child, const RNode* rep, int count, size_t pos, const Cont& k) {
        bool can_more = rep->max < 0 || count < rep->max;
        auto more = [&]() {
            return match(child, pos, [&](size_t p) {
                if (p == pos) return false;  // zero-width: stop to avoid infinite loop
                return match_repeat(child, rep, count + 1, p, k);
            });
        };
        if (rep->greedy) {
            if (can_more && more()) return true;
            if (count >= rep->min) return k(pos);
            return false;
        } else {
            if (count >= rep->min && k(pos)) return true;
            if (can_more) return more();
            return false;
        }
    }
};

}  // namespace

Match search(const std::u16string& pattern, const std::u16string& input,
             size_t start, bool ignore_case, bool multiline) {
    Match result;
    Parser parser{pattern};
    NodePtr root = parser.parse();
    if (!parser.ok || !root) return result;

    Matcher m{input, ignore_case, multiline, {}};
    int ngroups = parser.group_count;
    for (size_t i = start; i <= input.size(); ++i) {
        m.groups.assign(static_cast<size_t>(ngroups) + 1, {-1, -1});
        bool ok = m.match(root.get(), i, [&](size_t end) {
            m.groups[0] = {static_cast<long>(i), static_cast<long>(end)};
            result.index = i; result.end = end;
            return true;
        });
        if (ok) { result.matched = true; result.groups = m.groups; return result; }
    }
    return result;
}

bool valid(const std::u16string& pattern) {
    Parser parser{pattern};
    NodePtr root = parser.parse();
    return parser.ok && root != nullptr;
}

}  // namespace malibu::js::runtime::regex
