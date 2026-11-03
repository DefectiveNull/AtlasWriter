#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Document metadata from a ```meta block written in CCL (see config/Ccl.h):
//
//   ```meta
//   title = My Notes
//   tags =
//     = work
//     = ideas
//   date = 2026-11-02
//   ```
//
// The block only counts when it is the first thing in the file (blank lines
// before it are fine); anywhere else it is an ordinary code block. It only
// depends on the text, so anything holding a Markdown file can read it: the
// app, the Explorer property handler, link cards.
namespace md {

struct Meta
{
    bool present = false;    // the file starts with a ```meta block
    std::string error;       // CCL parse error, "line N: message" (fields stay empty)
    std::string title;
    std::string author;
    std::string description;
    std::string date;
    std::string created;
    std::string updated;
    std::vector<std::string> tags; // `tags =` as a CCL list, or `tags = a, b`
    std::vector<std::pair<std::string, std::string>> other; // any other field, in order

    // The date worth showing: updated, else date, else created.
    const std::string& shownDate() const;
};

// Where a block at the top of a file sits.
struct TopBlock
{
    size_t contentBegin = 0; // offset of the first line inside the fences
    size_t contentEnd = 0;   // offset just past the last line inside (before its '\n')
    size_t end = 0;          // offset just past the closing fence's line
    size_t lines = 0;        // lines the block takes, fences included
};

// A ```meta (or ~~~meta) block at the top, closed by a matching fence.
bool findMetaBlock(std::string_view text, TopBlock& out);

// YAML front matter: "---" on the very first line, closed by "---" or "...".
bool findFrontMatter(std::string_view text, TopBlock& out);

// Reads the meta block, if any.
Meta parseMeta(std::string_view text);

} // namespace md
