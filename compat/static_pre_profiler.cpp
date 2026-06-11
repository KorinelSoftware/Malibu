// compat/static_pre_profiler.cpp
// Single-pass scanner detecting environment-sniffing probes (dot / bracket /
// destructuring) without parsing or rewriting the source.

#include "malibu/compat/static_pre_profiler.h"
#include "malibu/diagnostics/diagnostic_log.h"

#include <array>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace malibu::compat {
namespace {

struct Probe { const char* object; const char* property; const char* id; };

constexpr std::array<Probe, 7> kProbes = {{
    {"window",    "chrome",         "window.chrome"},
    {"navigator", "userAgent",      "navigator.userAgent"},
    {"navigator", "vendor",         "navigator.vendor"},
    {"window",    "opr",            "window.opr"},
    {"window",    "InstallTrigger", "window.InstallTrigger"},
    {"document",  "documentMode",   "document.documentMode"},
    {"window",    "StyleMedia",     "window.StyleMedia"},
}};

// (object, property) -> id
const std::unordered_map<std::string, std::string>& pair_map() {
    static const std::unordered_map<std::string, std::string> m = [] {
        std::unordered_map<std::string, std::string> r;
        for (const auto& p : kProbes) r[std::string(p.object) + "." + p.property] = p.id;
        return r;
    }();
    return m;
}
// property -> list of (object, id) for destructuring/bracket-without-object.
const std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>>& prop_map() {
    static const auto m = [] {
        std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> r;
        for (const auto& p : kProbes) r[p.property].push_back({p.object, p.id});
        return r;
    }();
    return m;
}
bool is_object_name(const std::string& s) {
    return s == "window" || s == "navigator" || s == "document";
}

enum class TK { Ident, Str, Dot, LBracket, RBracket, LBrace, RBrace, Eq, Other };
struct Tok { TK kind; std::string text; };

bool id_start(char c) { return std::isalpha((unsigned char)c) || c == '_' || c == '$'; }
bool id_part(char c)  { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; }

}  // namespace

const std::vector<std::string>& StaticPreProfiler::known_probes() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> r;
        for (const auto& p : kProbes) r.emplace_back(p.id);
        return r;
    }();
    return v;
}

ProbeReport StaticPreProfiler::scan(std::u16string_view source) {
    ProbeReport report;

    // Narrow to bytes; flag binary/untokenizable input.
    std::string src;
    src.reserve(source.size());
    size_t control = 0;
    for (char16_t c : source) {
        if (c == u'\0') { report.tokenization_failed = true; }
        if (c < 0x20 && c != u'\t' && c != u'\n' && c != u'\r' && c != u'\f' && c != u'\v')
            ++control;
        src.push_back(static_cast<char>(c & 0xFF));
    }
    if (report.tokenization_failed ||
        (!src.empty() && control * 4 > src.size())) {  // >25% control bytes
        report.tokenization_failed = true;
        report.detected_probes.clear();
        MALIBU_LOG(WARNING, "compat", "StaticPreProfiler: source not tokenizable");
        return report;
    }

    // Tokenize (single pass).
    std::vector<Tok> toks;
    size_t i = 0, n = src.size();
    while (i < n) {
        char c = src[i];
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') { while (i < n && src[i] != '\n') ++i; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') { i += 2; while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) ++i; i += 2; continue; }
        if (id_start(c)) {
            size_t s = i; while (i < n && id_part(src[i])) ++i;
            toks.push_back({TK::Ident, src.substr(s, i - s)});
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c; ++i; std::string val;
            while (i < n && src[i] != q) { if (src[i] == '\\' && i + 1 < n) { ++i; } val.push_back(src[i]); ++i; }
            ++i;
            toks.push_back({TK::Str, val});
            continue;
        }
        switch (c) {
            case '.': toks.push_back({TK::Dot, "."}); break;
            case '[': toks.push_back({TK::LBracket, "["}); break;
            case ']': toks.push_back({TK::RBracket, "]"}); break;
            case '{': toks.push_back({TK::LBrace, "{"}); break;
            case '}': toks.push_back({TK::RBrace, "}"}); break;
            case '=': toks.push_back({TK::Eq, "="}); break;
            default:  toks.push_back({TK::Other, std::string(1, c)}); break;
        }
        ++i;
    }

    std::unordered_set<std::string> found;
    auto record = [&](const std::string& id) { found.insert(id); };

    for (size_t t = 0; t < toks.size(); ++t) {
        const Tok& tok = toks[t];

        // 1) dot notation: obj . prop
        if (tok.kind == TK::Ident && is_object_name(tok.text) &&
            t + 2 < toks.size() && toks[t + 1].kind == TK::Dot &&
            toks[t + 2].kind == TK::Ident) {
            auto it = pair_map().find(tok.text + "." + toks[t + 2].text);
            if (it != pair_map().end()) record(it->second);
        }
        // 2) bracket notation: obj [ "prop" ]
        if (tok.kind == TK::Ident && is_object_name(tok.text) &&
            t + 2 < toks.size() && toks[t + 1].kind == TK::LBracket &&
            toks[t + 2].kind == TK::Str) {
            auto it = pair_map().find(tok.text + "." + toks[t + 2].text);
            if (it != pair_map().end()) record(it->second);
        }
        // 3) destructuring: } = obj  (collect idents back to matching {)
        if (tok.kind == TK::RBrace && t + 2 < toks.size() &&
            toks[t + 1].kind == TK::Eq && toks[t + 2].kind == TK::Ident &&
            is_object_name(toks[t + 2].text)) {
            const std::string& obj = toks[t + 2].text;
            int depth = 1;
            for (size_t b = t; b-- > 0;) {
                if (toks[b].kind == TK::RBrace) ++depth;
                else if (toks[b].kind == TK::LBrace) { if (--depth == 0) break; }
                else if (depth == 1 && toks[b].kind == TK::Ident) {
                    auto it = prop_map().find(toks[b].text);
                    if (it != prop_map().end()) {
                        for (const auto& [o, id] : it->second)
                            if (o == obj) record(id);
                    }
                }
            }
        }
    }

    report.detected_probes.assign(found.begin(), found.end());
    return report;
}

} // namespace malibu::compat
