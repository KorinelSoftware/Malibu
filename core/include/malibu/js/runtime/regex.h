#pragma once
// core/include/malibu/js/runtime/regex.h
// A compact backtracking regular-expression engine over UTF-16 code units.
// Supports the common ECMAScript subset: literals, '.', char classes
// ([...], [^...], ranges), predefined classes (\d \D \w \W \s \S), anchors
// (^ $), word boundaries (\b \B), groups ((...) and (?:...)), alternation (|),
// and quantifiers (* + ? {n} {n,} {n,m}) with greedy/lazy variants. Flags: i
// (ignore case), m (multiline). Not a full spec engine (no lookbehind,
// backreferences, named groups, or Unicode property escapes) — enough for the
// regexes real web bundles use at boot.

#include <cstddef>
#include <string>
#include <vector>

namespace malibu::js::runtime::regex {

struct Match {
    bool   matched = false;
    size_t index   = 0;   // start offset of the whole match
    size_t end     = 0;   // one-past-end offset of the whole match
    // Capture groups (1-based; index 0 mirrors the whole match). {-1,-1} if a
    // group did not participate.
    std::vector<std::pair<long, long>> groups;
};

// Finds the first match at or after `start`. Returns matched=false if none.
Match search(const std::u16string& pattern, const std::u16string& input,
             size_t start, bool ignore_case, bool multiline);

// True if `pattern` compiles without error (used to validate `new RegExp`).
bool valid(const std::u16string& pattern);

}  // namespace malibu::js::runtime::regex
