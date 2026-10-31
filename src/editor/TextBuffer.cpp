#include "TextBuffer.h"

#include "text/Utf8.h"

#include <algorithm>
#include <cstring>

namespace {
constexpr size_t kMinGap = 4096;
}

TextBuffer::TextBuffer()
{
    reserveGap(kMinGap);
}

void TextBuffer::setText(std::string_view text)
{
    data_.assign(text.begin(), text.end());
    gapStart_ = gapEnd_ = data_.size();
    reserveGap(kMinGap);

    lineStarts_.assign(1, 0);
    if (!text.empty()) {
        const char* const begin = text.data();
        const char* const end = begin + text.size();
        for (const char* p = begin;
             (p = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)))) != nullptr;) {
            ++p;
            lineStarts_.push_back(static_cast<size_t>(p - begin));
        }
    }
    ++version_;
}

std::string TextBuffer::toString() const
{
    return substr(0, size());
}

std::string TextBuffer::substr(size_t pos, size_t len) const
{
    const size_t end = pos + len;
    std::string out;
    out.reserve(len);
    if (pos < gapStart_)
        out.append(data_.data() + pos, std::min(end, gapStart_) - pos);
    if (end > gapStart_) {
        const size_t from = std::max(pos, gapStart_);
        out.append(data_.data() + from + gapLength(), end - from);
    }
    return out;
}

void TextBuffer::insert(size_t pos, std::string_view text)
{
    if (text.empty())
        return;

    const size_t line = lineOf(pos);
    moveGap(pos);
    reserveGap(text.size());
    std::memcpy(data_.data() + gapStart_, text.data(), text.size());
    gapStart_ += text.size();

    for (size_t i = line + 1; i < lineStarts_.size(); ++i)
        lineStarts_[i] += text.size();

    std::vector<size_t> added; // stays unallocated unless the text has newlines
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n')
            added.push_back(pos + i + 1);
    }
    lineStarts_.insert(lineStarts_.begin() + static_cast<std::ptrdiff_t>(line) + 1, added.begin(), added.end());
    ++version_;
}

void TextBuffer::erase(size_t pos, size_t len)
{
    len = std::min(len, size() - pos);
    if (len == 0)
        return;

    const size_t end = pos + len;
    const size_t line = lineOf(pos);
    const auto first = lineStarts_.begin() + static_cast<std::ptrdiff_t>(line) + 1;
    const auto last = std::upper_bound(first, lineStarts_.end(), end);
    for (auto it = lineStarts_.erase(first, last); it != lineStarts_.end(); ++it)
        *it -= len;

    moveGap(pos);
    gapEnd_ += len;
    ++version_;
}

size_t TextBuffer::lineEnd(size_t line) const
{
    return line + 1 < lineStarts_.size() ? lineStarts_[line + 1] - 1 : size();
}

size_t TextBuffer::lineOf(size_t pos) const
{
    const auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), pos);
    return static_cast<size_t>(it - lineStarts_.begin()) - 1;
}

std::string_view TextBuffer::line(size_t line, std::string& scratch) const
{
    const size_t start = lineStart(line);
    const size_t end = lineEnd(line);
    if (end <= gapStart_)
        return {data_.data() + start, end - start};
    if (start >= gapStart_)
        return {data_.data() + start + gapLength(), end - start};

    scratch.assign(data_.data() + start, gapStart_ - start);
    scratch.append(data_.data() + gapEnd_, end - gapStart_);
    return scratch;
}

size_t TextBuffer::prevCodepoint(size_t pos) const
{
    if (pos == 0)
        return 0;
    --pos;
    while (pos > 0 && utf8::isContinuation(at(pos)))
        --pos;
    return pos;
}

size_t TextBuffer::nextCodepoint(size_t pos) const
{
    const size_t n = size();
    if (pos >= n)
        return n;
    ++pos;
    while (pos < n && utf8::isContinuation(at(pos)))
        ++pos;
    return pos;
}

void TextBuffer::moveGap(size_t pos)
{
    if (pos < gapStart_) {
        const size_t n = gapStart_ - pos;
        std::memmove(data_.data() + gapEnd_ - n, data_.data() + pos, n);
        gapStart_ -= n;
        gapEnd_ -= n;
    } else if (pos > gapStart_) {
        const size_t n = pos - gapStart_;
        std::memmove(data_.data() + gapStart_, data_.data() + gapEnd_, n);
        gapStart_ += n;
        gapEnd_ += n;
    }
}

void TextBuffer::reserveGap(size_t needed)
{
    if (gapLength() >= needed)
        return;

    // Grow proportionally to the text so repeated inserts stay amortized O(1).
    const size_t textSize = size();
    const size_t newGap = std::max({needed, kMinGap, textSize / 2});
    const size_t tail = data_.size() - gapEnd_;

    std::vector<char> grown(textSize + newGap);
    if (gapStart_ > 0)
        std::memcpy(grown.data(), data_.data(), gapStart_);
    if (tail > 0)
        std::memcpy(grown.data() + gapStart_ + newGap, data_.data() + gapEnd_, tail);
    data_.swap(grown);
    gapEnd_ = gapStart_ + newGap;
}

std::string normalizeNewlines(std::string_view text, bool* hadCrlf)
{
    bool crlf = false;
    std::string out;
    if (text.find('\r') == std::string_view::npos) {
        out.assign(text);
    } else {
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] != '\r') {
                out.push_back(text[i]);
                continue;
            }
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                crlf = true;
                ++i;
            }
            out.push_back('\n');
        }
    }
    if (hadCrlf)
        *hadCrlf = crlf;
    return out;
}
