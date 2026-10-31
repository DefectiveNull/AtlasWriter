#pragma once

#include <stb_truetype.h>

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

// A loaded TrueType font at one pixel size.
//
// Copies share the raw font file bytes (stb_truetype reads glyph data
// directly out of them), so one file can be used at several sizes cheaply:
// copy the Font, then call setPixelSize() on the copy.
class Font
{
public:
    bool loadFromFile(const char* path);

    // Size of the em square in pixels (what editors call "font size").
    void setPixelSize(float px);

    float pixelSize() const { return pixelSize_; }
    float scale() const { return scale_; }

    // Rounded to whole pixels so baselines land on the pixel grid.
    int ascent() const { return ascent_; }
    int lineHeight() const { return lineHeight_; }

    int glyphIndex(uint32_t codepoint) const;
    float advance(int glyph) const;

    // Advance of a codepoint, cached; layout never needs to rasterize.
    float advanceOf(uint32_t codepoint) const;
    // Upper bound for any glyph's advance plus kerning (from the hhea table
    // and the largest positive ASCII kerning pair).
    float maxAdvance() const { return maxAdvance_; }

    // Pair kerning from the GPOS or kern table, in pixels; added between
    // `left` and `right`. Monospace fonts should turn it off so columns stay
    // aligned. Printable ASCII pairs come from a table built once per font
    // file (shared by copies); other pairs are looked up once and cached.
    void setKerning(bool enabled) { kerning_ = enabled && kernAscii_ != nullptr; }
    bool kerning() const { return kerning_; }
    float kern(uint32_t left, uint32_t right) const
    {
        if (!kerning_)
            return 0.0f;
        if (left < 128 && right < 128)
            return (*kernAscii_)[left * 128 + right] * scale_;
        return kernSlow(left, right);
    }

    const stbtt_fontinfo& info() const { return info_; }

private:
    using KernTable = std::array<int16_t, 128 * 128>; // font units

    void buildKernTable();
    float kernSlow(uint32_t left, uint32_t right) const;

    std::shared_ptr<const std::vector<unsigned char>> data_;
    std::shared_ptr<const KernTable> kernAscii_; // null when the font has no kerning
    bool kerning_ = false;
    int maxKern_ = 0; // largest positive ASCII pair, in font units
    stbtt_fontinfo info_{};
    float pixelSize_ = 0.0f;
    float scale_ = 0.0f;
    int ascent_ = 0;
    int lineHeight_ = 0;
    float maxAdvance_ = 0.0f;

    std::array<float, 128> asciiAdvance_{};
    mutable std::unordered_map<uint32_t, float> advanceCache_;
    mutable std::unordered_map<uint64_t, int16_t> kernCache_; // non-ASCII pairs, font units
};
