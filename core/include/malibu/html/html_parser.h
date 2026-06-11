#pragma once
// core/include/malibu/html/html_parser.h
// HTML5 tokenizer + tree construction (Task 28). Builds a NodeTable-backed DOM
// under a Document, captures inline <script> sources and <style> stylesheets,
// and handles void/raw-text elements, attributes, comments, doctype, and
// character entities. (Subset of the full adoption-agency algorithm; robust for
// well-formed real-world markup.)

#include <string>
#include <string_view>
#include <vector>

#include "malibu/dom/document.h"

namespace malibu::html {

// A <script> in document order: either inline source or an external `src` URL.
struct ScriptItem {
    bool          external = false;
    std::u16string code;   // inline source (when !external)
    std::u16string src;    // URL (when external)
};

struct ParsedDocument {
    std::vector<std::u16string> scripts;        // inline <script> contents, in order
    std::vector<std::u16string> stylesheets;    // <style> contents, in order
    std::vector<ScriptItem>     script_items;   // all <script>s (inline + external) in order
    std::vector<std::u16string> external_styles;// <link rel=stylesheet> hrefs, in order
};

class HTMLParser {
public:
    // Parses `html` into `tree`, appending elements under the document root.
    ParsedDocument parse(std::u16string_view html, malibu::dom::DOMTree& tree);

    // Fragment parse (for innerHTML / insertAdjacentHTML): appends the parsed
    // nodes under `context` instead of the document root.
    ParsedDocument parse_fragment(std::u16string_view html, malibu::dom::DOMTree& tree,
                                  malibu::NodeHandle context);
};

} // namespace malibu::html
