#pragma once

#include <cstdint>

namespace utf8 {

inline bool isContinuation(char c)
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// Decodes one codepoint and advances `p`. Invalid or truncated sequences
// decode as U+FFFD and advance a single byte, so callers always make progress.
inline uint32_t decode(const char*& p, const char* end)
{
    const auto* s = reinterpret_cast<const unsigned char*>(p);
    const unsigned char c = s[0];
    if (c < 0x80) {
        p += 1;
        return c;
    }

    int length = 0;
    uint32_t cp = 0;
    uint32_t minimum = 0;
    if ((c & 0xE0) == 0xC0) {
        length = 2, cp = c & 0x1Fu, minimum = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        length = 3, cp = c & 0x0Fu, minimum = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        length = 4, cp = c & 0x07u, minimum = 0x10000;
    } else {
        p += 1;
        return 0xFFFD;
    }

    if (end - p < length) {
        p += 1;
        return 0xFFFD;
    }
    for (int i = 1; i < length; ++i) {
        if ((s[i] & 0xC0) != 0x80) {
            p += 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        p += 1;
        return 0xFFFD;
    }
    p += length;
    return cp;
}

} // namespace utf8
