#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A small, forgiving Markdown parser: turns source text into a flat list of
// blocks with styled inline spans, which is all the rendered view needs.
//
// Supported: ATX headings, paragraphs, emphasis (* and _), strong, ~~strike~~,
// `code`, fenced code blocks, block quotes (nested), bullet / ordered / task
// lists (nested by indentation), horizontal rules, links, images (shown by
// their alt text), autolinks, backslash escapes, entities, GitHub pipe tables,
// and HTML (block and inline; see Html.h). Not yet: setext headings,
// indented code blocks.
namespace md {

enum SpanStyle : uint8_t {
    kBold = 1 << 0,
    kItalic = 1 << 1,
    kCode = 1 << 2,
    kLink = 1 << 3,
    kStrike = 1 << 4,
    kImage = 1 << 5,
    kUnderline = 1 << 6,
};

struct Span
{
    std::string text; // for images: the alt text
    uint8_t style = 0;
    std::string url;  // links and images: the destination / source
    int width = 0;    // images: requested size in CSS pixels (HTML width/height), 0 = natural
    int height = 0;
};

enum class BlockType { Paragraph, Heading, ListItem, Code, Rule, Table };

enum class Align : uint8_t { Default, Left, Center, Right };

struct TableRow
{
    std::vector<std::vector<Span>> cells;
    bool header = false;
};

struct Block
{
    BlockType type = BlockType::Paragraph;
    int level = 0;        // heading 1-6, or list nesting depth (0 = top level)
    int quote = 0;        // block quote depth
    int task = -1;        // list items: -1 not a task, 0 open, 1 done
    std::string marker;   // list items: "1." for ordered, empty for bullets
    size_t sourceLine = 0;
    std::vector<Span> spans; // inline content (Paragraph, Heading, ListItem)
    std::string code;        // Code: the lines, '\n'-separated

    // Table: rows of cells; header rows come first. align has one entry per
    // column (may be shorter than the widest row: missing means Default).
    std::vector<TableRow> rows;
    std::vector<Align> align;
};

std::vector<Block> parse(std::string_view text);
std::vector<Span> parseInline(std::string_view text);

} // namespace md
