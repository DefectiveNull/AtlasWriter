#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// UTF-8 text stored in a gap buffer, plus an index of line start offsets.
//
// Edits at the gap (where the user is typing) are amortized O(1); moving the
// gap costs a memmove of the distance moved. The line index is updated
// incrementally on every edit, so line lookups never rescan the text.
//
// Every offset is a byte offset into the logical (gap-free) text, and lines
// are separated by '\n' only (see normalizeNewlines()).
class TextBuffer
{
public:
    TextBuffer();

    void setText(std::string_view text);
    std::string toString() const;
    std::string substr(size_t pos, size_t len) const;

    void insert(size_t pos, std::string_view text);
    void erase(size_t pos, size_t len);

    size_t size() const { return data_.size() - gapLength(); }
    char at(size_t pos) const { return data_[pos < gapStart_ ? pos : pos + gapLength()]; }

    size_t lineCount() const { return lineStarts_.size(); }
    size_t lineStart(size_t line) const { return lineStarts_[line]; }
    size_t lineEnd(size_t line) const; // offset of the line's '\n', or size() on the last line
    size_t lineOf(size_t pos) const;

    // Contiguous view of a line without its '\n'. Copies into `scratch` only
    // when the line straddles the gap.
    std::string_view line(size_t line, std::string& scratch) const;

    size_t prevCodepoint(size_t pos) const;
    size_t nextCodepoint(size_t pos) const;

    // Incremented on every change; compare against a saved value to detect edits.
    uint64_t version() const { return version_; }

private:
    size_t gapLength() const { return gapEnd_ - gapStart_; }
    void moveGap(size_t pos);
    void reserveGap(size_t needed);

    std::vector<char> data_;
    size_t gapStart_ = 0;
    size_t gapEnd_ = 0;
    std::vector<size_t> lineStarts_{0};
    uint64_t version_ = 0;
};

// Converts CRLF and lone CR line endings to LF.
std::string normalizeNewlines(std::string_view text, bool* hadCrlf = nullptr);
