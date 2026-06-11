#pragma once
// core/include/malibu/css/selector/selector.h
// CSS Selectors Level 4 (subset): parsing into a structured form, specificity
// calculation, and matching against the DOM. Supports type/universal/class/id,
// attribute selectors, the descendant/child/adjacent/general-sibling
// combinators, and a useful set of pseudo-classes.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "malibu/types.h"

namespace malibu::dom { class DOMTree; class Document; }

namespace malibu::css {

struct Specificity {
    uint32_t a = 0;  // id selectors
    uint32_t b = 0;  // class / attribute / pseudo-class selectors
    uint32_t c = 0;  // type / pseudo-element selectors

    [[nodiscard]] bool operator<(const Specificity& o) const noexcept {
        if (a != o.a) return a < o.a;
        if (b != o.b) return b < o.b;
        return c < o.c;
    }
    bool operator==(const Specificity&) const noexcept = default;
};

struct AttrSelector {
    enum class Op : uint8_t { Exists, Equals, Includes, Prefix, Suffix, Substring };
    Op             op = Op::Exists;
    std::u16string name;
    std::u16string value;
};

struct PseudoClass {
    std::u16string name;   // e.g. "first-child", "nth-child", "hover"
    std::u16string arg;    // e.g. "odd", "even", "3"
};

struct CompoundSelector {
    bool                        universal = false;
    std::u16string              tag;       // lowercased; empty = any
    std::u16string              id;
    std::vector<std::u16string> classes;
    std::vector<AttrSelector>   attrs;
    std::vector<PseudoClass>    pseudos;
};

enum class PseudoElement : uint8_t {
    None,
    Before,
    After,
    Unsupported,
};

enum class Combinator : uint8_t { Descendant, Child, AdjacentSibling, GeneralSibling };

struct ComplexSelector {
    // steps[0].first (combinator) is unused; steps[i] relates to steps[i-1].
    std::vector<std::pair<Combinator, CompoundSelector>> steps;
    Specificity specificity;
    PseudoElement pseudo_element = PseudoElement::None;
    bool        valid = false;
};

// Parses a single complex selector (no top-level commas).
ComplexSelector parse_selector(std::u16string_view text);

// Matches a complex selector against `node` within `doc`.
bool matches(malibu::dom::Document& doc, malibu::NodeHandle node, const ComplexSelector& sel);

} // namespace malibu::css
