#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// CCL, the Categorical Configuration Language:
// https://chshersh.com/blog/2025-01-06-the-most-elegant-configuration-language.html
//
//   key = value             every entry; the first '=' splits it, both sides trimmed
//   = item                  empty keys and values are fine (lists)
//   /= a comment            an entry whose key is "/" is a comment
//   section =               lines indented deeper than the entry's key
//     nested = entries      continue its value, which is itself CCL
//
// Everything is a string; meaning is up to the reader. Parsing is done to
// the fixed point: nested values are parsed again, and the result is a tree
// where duplicate keys merge (so `port = 1` and `port = 2` become one
// `port` with children "1" and "2"). That merge is associative with the
// empty config as identity, so configs compose (defaults + user file).
//
// Choices where the post leaves room:
//  - A value is nested CCL only when it starts on the lines after its key
//    (`key =` then an indented block). A value on the key's own line is
//    always a plain string, so `zoom in = Ctrl+=` stays "Ctrl+=".
//  - Tabs count as one column of indentation, like a space.
namespace ccl {

struct Entry
{
    std::string key;
    std::string value;
    size_t line = 0;     // 1-based line of the key
    bool nested = false; // value is an indented block below the key
};

struct Error
{
    size_t line = 0;
    std::string message;
};

// One level: the entries of `text`, in order, duplicates kept.
bool parseEntries(std::string_view text, std::vector<Entry>& out, Error* error, size_t firstLine = 1);

// A node of the fixed-point tree: ordered children, keys unique.
class Config
{
public:
    static bool parse(std::string_view text, Config& out, Error* error);

    const Config* find(std::string_view key) const;
    // The value of a scalar entry: its only child's key ("" if it has none
    // or several).
    std::string_view value() const;
    const std::vector<std::pair<std::string, Config>>& children() const { return children_; }
    bool empty() const { return children_.empty(); }

    // Recursive union: shared keys merge their subtrees.
    void merge(const Config& other);
    Config& child(std::string_view key); // adds it if missing

private:
    static bool build(const std::vector<Entry>& entries, Config& out, Error* error);

    std::vector<std::pair<std::string, Config>> children_;
};

} // namespace ccl
