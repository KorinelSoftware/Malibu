#pragma once
// core/include/malibu/css/parser/css_parser.h
// CSS Syntax Level 3 tokenizer + parser (Task 10 / Requirement 6.1, 6.2).
// Malformed declarations are discarded, logged with their source location, and
// parsing continues.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace malibu::css {

struct SourceLocation {
    uint32_t line = 1;
    uint32_t column = 1;
};

struct Declaration {
    std::u16string property;   // lowercased property name
    std::u16string value;      // serialized value (trimmed, without !important)
    bool           important = false;
    SourceLocation loc;
};

struct Selector {
    std::u16string text;       // a single complex selector (no commas)
};

struct Rule {
    std::vector<Selector>     selectors;
    std::vector<Declaration>  declarations;
    std::u16string            media;   // enclosing @media condition ("" = always)
};

struct StyleSheet {
    std::vector<Rule> rules;
};

class CSSParser {
public:
    // Parses CSS source text into a structured stylesheet. Malformed
    // declarations are dropped and logged; the rest of the sheet is preserved.
    StyleSheet parse(std::u16string_view source);
};

// CSS Conditional Rules capability queries used by the Web-exposed CSS
// namespace. These report the syntax and values the current engine can
// actually parse and apply.
bool supports_property_value(std::u16string_view property,
                             std::u16string_view value);
bool supports_selector(std::u16string_view selector);
bool supports_condition(std::u16string_view condition);

} // namespace malibu::css
