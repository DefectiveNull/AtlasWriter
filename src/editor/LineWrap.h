#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class TextBuffer;
class TextRenderer;

// Maps logical lines to visual rows for soft word wrap.
//
// Only a row count per line is stored; where a line actually breaks is
// recomputed on demand (rowStarts()), which is cheap and only ever needed
// for the handful of lines on screen. Edits re-wrap just the lines they
// touched, and the line -> first row prefix sums are rebuilt lazily, once
// per batch of edits.
//
// When disabled, every line is exactly one row and nothing is stored, so
// the no-wrap case costs nothing.
class LineWrap
{
public:
    explicit LineWrap(const TextBuffer& buffer);

    void disable();
    // Enables wrapping and wraps every line to `width` pixels.
    void rebuild(const TextRenderer& text, float width);
    // Lines [first, first + oldCount) were replaced by [first, first + newCount).
    void update(size_t first, size_t oldCount, size_t newCount);

    bool enabled() const { return text_ != nullptr; }
    float width() const { return width_; }

    size_t rowCount() const;
    size_t rowsIn(size_t line) const;
    size_t firstRow(size_t line) const;
    size_t lineAtRow(size_t row) const;

    // Byte offsets (relative to the line start) where each row of `lineText`
    // begins; out[0] is always 0.
    void rowStarts(std::string_view lineText, std::vector<size_t>& out) const;

private:
    uint32_t countRows(std::string_view lineText) const;
    void ensurePrefix() const;

    const TextBuffer& buffer_;
    const TextRenderer* text_ = nullptr;
    float width_ = 0.0f;
    float fastWidth_ = 0.0f; // widest any single byte can be (see countRows)

    std::vector<uint32_t> rows_;            // rows per line
    mutable std::vector<size_t> firstRow_;  // prefix sums, size = lines + 1
    mutable bool prefixDirty_ = true;
    std::string scratch_;
};
