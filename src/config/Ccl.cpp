#include "Ccl.h"

#include <algorithm>

namespace ccl {

namespace {

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && isSpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && (isSpace(s.back()) || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

size_t indentOf(std::string_view line)
{
    size_t n = 0;
    while (n < line.size() && (line[n] == ' ' || line[n] == '\t'))
        ++n;
    return n;
}

} // namespace

bool parseEntries(std::string_view text, std::vector<Entry>& out, Error* error, size_t firstLine)
{
    out.clear();
    long long keyIndent = -1; // indentation of this level's keys (the first key's)
    size_t lineNo = firstLine;

    for (size_t pos = 0; pos <= text.size(); ++lineNo) {
        const size_t newline = std::min(text.find('\n', pos), text.size());
        std::string_view line = text.substr(pos, newline - pos);
        pos = newline + 1;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);

        if (trim(line).empty()) {
            if (!out.empty())
                out.back().value += '\n'; // blank lines inside a value are kept
            continue;
        }
        const auto indent = static_cast<long long>(indentOf(line));
        if (keyIndent < 0)
            keyIndent = indent;

        if (indent > keyIndent && !out.empty()) {
            // Continuation: kept verbatim so a nested block keeps its own indentation.
            out.back().value += '\n';
            out.back().value.append(line);
            continue;
        }

        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            if (error)
                *error = {lineNo, "expected \"key = value\""};
            return false;
        }
        Entry entry;
        entry.key.assign(trim(line.substr(0, eq)));
        entry.value.assign(trim(line.substr(eq + 1)));
        entry.line = lineNo;
        entry.nested = entry.value.empty();
        out.push_back(std::move(entry));
    }

    for (Entry& entry : out) {
        // Trailing blank lines go; a block value starts at its first line
        // (whose indentation is kept, for the nested parse).
        while (!entry.value.empty() && (isSpace(entry.value.back()) || entry.value.back() == '\n'))
            entry.value.pop_back();
        if (entry.nested) {
            size_t start = 0;
            while (start < entry.value.size() && entry.value[start] == '\n')
                ++start;
            entry.value.erase(0, start);
            entry.nested = !entry.value.empty();
        }
    }
    return true;
}

bool Config::parse(std::string_view text, Config& out, Error* error)
{
    std::vector<Entry> entries;
    if (!parseEntries(text, entries, error))
        return false;
    out = Config{};
    return build(entries, out, error);
}

bool Config::build(const std::vector<Entry>& entries, Config& out, Error* error)
{
    for (const Entry& entry : entries) {
        if (entry.key == "/")
            continue; // comment
        Config value;
        if (entry.nested) {
            std::vector<Entry> inner;
            // Block values start on the line after their key.
            if (!parseEntries(entry.value, inner, error, entry.line + 1) || !build(inner, value, error))
                return false;
        } else if (!entry.value.empty()) {
            value.children_.emplace_back(entry.value, Config{});
        }
        out.child(entry.key).merge(value);
    }
    return true;
}

const Config* Config::find(std::string_view key) const
{
    for (const auto& [k, v] : children_) {
        if (k == key)
            return &v;
    }
    return nullptr;
}

std::string_view Config::value() const
{
    return children_.size() == 1 ? std::string_view(children_[0].first) : std::string_view();
}

Config& Config::child(std::string_view key)
{
    for (auto& [k, v] : children_) {
        if (k == key)
            return v;
    }
    children_.emplace_back(std::string(key), Config{});
    return children_.back().second;
}

void Config::merge(const Config& other)
{
    for (const auto& [key, value] : other.children_)
        child(key).merge(value);
}

} // namespace ccl
