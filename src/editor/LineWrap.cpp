#include "LineWrap.h"

#include "editor/TextBuffer.h"
#include "text/Font.h"
#include "text/TextRenderer.h"

#include <algorithm>

LineWrap::LineWrap(const TextBuffer& buffer)
    : buffer_(buffer)
{
}

void LineWrap::disable()
{
    text_ = nullptr;
    width_ = 0.0f;
    rows_.clear();
    rows_.shrink_to_fit();
    firstRow_.clear();
    firstRow_.shrink_to_fit();
}

void LineWrap::rebuild(const TextRenderer& text, float width)
{
    text_ = &text;
    width_ = std::max(width, 1.0f);
    const Font& font = text.font();
    fastWidth_ = std::max(font.maxAdvance(), font.advanceOf(' ') * static_cast<float>(text.tabSize()));

    const size_t lines = buffer_.lineCount();
    rows_.resize(lines);
    for (size_t line = 0; line < lines; ++line)
        rows_[line] = countRows(buffer_.line(line, scratch_));
    prefixDirty_ = true;
}

void LineWrap::update(size_t first, size_t oldCount, size_t newCount)
{
    if (!enabled())
        return;
    const auto begin = rows_.begin() + static_cast<std::ptrdiff_t>(first);
    if (newCount > oldCount)
        rows_.insert(begin, newCount - oldCount, 0u);
    else if (oldCount > newCount)
        rows_.erase(begin, begin + static_cast<std::ptrdiff_t>(oldCount - newCount));
    for (size_t line = first; line < first + newCount; ++line)
        rows_[line] = countRows(buffer_.line(line, scratch_));
    prefixDirty_ = true;
}

uint32_t LineWrap::countRows(std::string_view lineText) const
{
    // A codepoint is at least one byte, so a line this short fits without
    // measuring it. That covers most lines of typical text.
    if (static_cast<float>(lineText.size()) * fastWidth_ <= width_)
        return 1;

    uint32_t rows = 1;
    size_t pos = 0;
    while (true) {
        const size_t n = text_->fit(lineText.substr(pos), width_);
        pos += n;
        if (pos >= lineText.size())
            return rows;
        ++rows;
    }
}

void LineWrap::rowStarts(std::string_view lineText, std::vector<size_t>& out) const
{
    out.clear();
    out.push_back(0);
    if (!enabled() || static_cast<float>(lineText.size()) * fastWidth_ <= width_)
        return;
    size_t pos = 0;
    while (true) {
        pos += text_->fit(lineText.substr(pos), width_);
        if (pos >= lineText.size())
            return;
        out.push_back(pos);
    }
}

void LineWrap::ensurePrefix() const
{
    if (!prefixDirty_)
        return;
    firstRow_.resize(rows_.size() + 1);
    size_t total = 0;
    for (size_t line = 0; line < rows_.size(); ++line) {
        firstRow_[line] = total;
        total += rows_[line];
    }
    firstRow_[rows_.size()] = total;
    prefixDirty_ = false;
}

size_t LineWrap::rowCount() const
{
    if (!enabled())
        return buffer_.lineCount();
    ensurePrefix();
    return firstRow_.back();
}

size_t LineWrap::rowsIn(size_t line) const
{
    return enabled() ? rows_[line] : 1;
}

size_t LineWrap::firstRow(size_t line) const
{
    if (!enabled())
        return line;
    ensurePrefix();
    return firstRow_[line];
}

size_t LineWrap::lineAtRow(size_t row) const
{
    if (!enabled())
        return std::min(row, buffer_.lineCount() - 1);
    ensurePrefix();
    // Last line whose first row is <= row.
    const auto it = std::upper_bound(firstRow_.begin(), firstRow_.end() - 1, row);
    return static_cast<size_t>(it - firstRow_.begin()) - 1;
}
