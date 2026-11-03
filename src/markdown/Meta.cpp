#include "Meta.h"

#include "config/Ccl.h"

#include <algorithm>

namespace md {

namespace {

bool isBlank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && isBlank(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && isBlank(s.back()))
        s.remove_suffix(1);
    return s;
}

// Walks `text` line by line from `pos`; false at the end.
bool nextLine(std::string_view text, size_t& pos, std::string_view& line, size_t& lineEnd)
{
    if (pos > text.size())
        return false;
    lineEnd = std::min(text.find('\n', pos), text.size());
    line = text.substr(pos, lineEnd - pos);
    return true;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return (x >= 'A' && x <= 'Z' ? x - 'A' + 'a' : x) == (y >= 'A' && y <= 'Z' ? y - 'A' + 'a' : y);
           });
}

// The lines between an opening line at `pos` (already matched) and the first
// line `closes` accepts.
template <typename Closes>
bool findClose(std::string_view text, size_t afterOpen, size_t linesBefore, Closes closes, TopBlock& out)
{
    size_t pos = afterOpen;
    size_t lines = linesBefore + 1;
    std::string_view line;
    size_t lineEnd = 0;
    while (pos <= text.size() && nextLine(text, pos, line, lineEnd)) {
        ++lines;
        if (closes(line)) {
            out.contentBegin = afterOpen;
            out.contentEnd = pos > afterOpen ? pos - 1 : afterOpen;
            out.end = std::min(lineEnd + 1, text.size());
            out.lines = lines;
            return true;
        }
        pos = lineEnd + 1;
    }
    return false;
}

void splitList(std::string_view s, std::vector<std::string>& out)
{
    while (!s.empty()) {
        const size_t comma = std::min(s.find(','), s.size());
        if (const std::string_view item = trim(s.substr(0, comma)); !item.empty())
            out.emplace_back(item);
        s.remove_prefix(std::min(comma + 1, s.size()));
    }
}

} // namespace

const std::string& Meta::shownDate() const
{
    return !updated.empty() ? updated : !date.empty() ? date : created;
}

bool findMetaBlock(std::string_view text, TopBlock& out)
{
    size_t pos = 0;
    size_t blankLines = 0;
    std::string_view line;
    size_t lineEnd = 0;
    while (nextLine(text, pos, line, lineEnd)) {
        if (!trim(line).empty())
            break;
        if (lineEnd >= text.size())
            return false; // only blank lines
        pos = lineEnd + 1;
        ++blankLines;
    }

    // ```meta or ~~~meta, at least three fence characters, no indentation.
    if (line.size() < 3 || (line[0] != '`' && line[0] != '~'))
        return false;
    const char ch = line[0];
    size_t length = 0;
    while (length < line.size() && line[length] == ch)
        ++length;
    if (length < 3 || !equalsIgnoreCase(trim(line.substr(length)), "meta"))
        return false;

    return findClose(text, lineEnd + 1, blankLines,
                     [&](std::string_view l) {
                         l = trim(l);
                         return l.size() >= length && std::all_of(l.begin(), l.end(), [&](char c) { return c == ch; });
                     },
                     out);
}

bool findFrontMatter(std::string_view text, TopBlock& out)
{
    size_t pos = 0;
    std::string_view line;
    size_t lineEnd = 0;
    if (!nextLine(text, pos, line, lineEnd) || trim(line) != "---" || lineEnd >= text.size())
        return false;
    return findClose(text, lineEnd + 1, 0,
                     [](std::string_view l) {
                         l = trim(l);
                         return l == "---" || l == "...";
                     },
                     out);
}

Meta parseMeta(std::string_view text)
{
    Meta meta;
    TopBlock block;
    if (!findMetaBlock(text, block))
        return meta;
    meta.present = true;

    const std::string_view body = text.substr(block.contentBegin, block.contentEnd - block.contentBegin);
    ccl::Config config;
    ccl::Error error;
    if (!ccl::Config::parse(body, config, &error)) {
        const size_t firstLine = static_cast<size_t>(std::count(text.begin(), text.begin() + block.contentBegin, '\n'));
        meta.error = "line " + std::to_string(firstLine + error.line) + ": " + error.message;
        return meta;
    }

    for (const auto& [key, value] : config.children()) {
        const std::string scalar(trim(value.value()));
        if (key == "title")
            meta.title = scalar;
        else if (key == "author")
            meta.author = scalar;
        else if (key == "description")
            meta.description = scalar;
        else if (key == "date")
            meta.date = scalar;
        else if (key == "created")
            meta.created = scalar;
        else if (key == "updated")
            meta.updated = scalar;
        else if (key == "tags") {
            // `tags = a, b` is one child; a CCL list is children under "".
            for (const auto& [item, rest] : value.children()) {
                if (!item.empty()) {
                    splitList(item, meta.tags);
                } else {
                    for (const auto& [tag, unused] : rest.children())
                        splitList(tag, meta.tags);
                }
            }
        } else if (!key.empty() && key != "/") {
            meta.other.emplace_back(key, scalar);
        }
    }
    return meta;
}

} // namespace md
