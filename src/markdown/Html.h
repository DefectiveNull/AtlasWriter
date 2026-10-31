#pragma once

#include "markdown/Markdown.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The HTML subset Markdown documents use in practice (READMEs, notes),
// mapped onto the same blocks and spans as Markdown so one renderer draws
// both. Nothing is executed or fetched: scripts, styles and comments are
// dropped, images show their alt text, unknown tags are ignored but their
// text is kept.
//
// Block tags: p, div and other sectioning tags, h1-h6, blockquote, ul/ol/li
// (with <input type="checkbox"> tasks), pre, hr, br, table/thead/tr/th/td
// (with align), details/summary (always open).
// Inline tags: b, strong, i, em, cite, code, kbd, samp, s, del, strike,
// u, ins, a, img.
namespace md {

struct Tag
{
    std::string name; // lowercase
    bool closing = false;
    bool selfClosing = false;
    std::string_view attributes; // raw text between the name and '>'
};

// Parses a tag starting at s[pos] == '<'. Returns its length, or 0 if the
// text there is not a tag. Comments, <!DOCTYPE>, <?...?> and CDATA parse as
// a Tag with an empty name.
size_t parseTag(std::string_view s, size_t pos, Tag& tag);

// Value of an attribute (entities decoded); `found` tells a missing
// attribute from an empty one.
std::string attribute(std::string_view attributes, std::string_view name, bool* found = nullptr);

// Decodes a character reference starting at s[pos] == '&' (named, &#123;
// or &#x1F600;), appending UTF-8 to `out`. Returns its length, or 0 if
// it isn't one.
size_t decodeEntity(std::string_view s, size_t pos, std::string& out);

// Style bit for an inline formatting tag (b, em, code, ...), or 0.
uint8_t inlineTagStyle(std::string_view name);

// Span for an <img>: alt text, src, and width/height when given in pixels.
Span imageSpan(const Tag& img, uint8_t style);

// Converts an HTML fragment into blocks, appended to `out`.
void parseHtml(std::string_view html, size_t sourceLine, int quote, std::vector<Block>& out);

// Whether a line opens a CommonMark HTML block: <pre>/<script>/<style>/
// <textarea> (end at the matching close tag), comments (end at "-->"),
// known block tags (end at a blank line), or any lone complete tag on its
// own line (end at a blank line; cannot interrupt a paragraph).
enum class HtmlBlockStart { None, UntilTag, UntilComment, UntilBlank, LoneTag };
HtmlBlockStart htmlBlockStart(std::string_view line, std::string& endMarker);

} // namespace md
