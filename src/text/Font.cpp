#include "Font.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

bool Font::loadFromFile(const char* path)
{
    size_t size = 0;
    void* bytes = SDL_LoadFile(path, &size);
    if (!bytes) {
        SDL_Log("Failed to load font '%s': %s", path, SDL_GetError());
        return false;
    }
    auto data = std::make_shared<std::vector<unsigned char>>(size);
    std::memcpy(data->data(), bytes, size);
    SDL_free(bytes);

    const int offset = stbtt_GetFontOffsetForIndex(data->data(), 0);
    if (offset < 0 || !stbtt_InitFont(&info_, data->data(), offset)) {
        SDL_Log("Failed to parse font '%s'", path);
        return false;
    }
    data_ = std::move(data);
    buildKernTable();
    return true;
}

void Font::buildKernTable()
{
    kernAscii_.reset();
    kernCache_.clear();
    maxKern_ = 0;
    if (info_.kern == 0 && info_.gpos == 0) {
        kerning_ = false;
        return;
    }

    // Pairs are in font units, so one table serves every pixel size.
    auto table = std::make_shared<KernTable>();
    table->fill(0);
    int glyphs[128] = {};
    for (int cp = 32; cp < 127; ++cp)
        glyphs[cp] = glyphIndex(static_cast<uint32_t>(cp));
    bool any = false;
    for (int left = 32; left < 127; ++left) {
        for (int right = 32; right < 127; ++right) {
            const int k = stbtt_GetGlyphKernAdvance(&info_, glyphs[left], glyphs[right]);
            if (k != 0) {
                (*table)[static_cast<size_t>(left) * 128 + static_cast<size_t>(right)] = static_cast<int16_t>(k);
                maxKern_ = std::max(maxKern_, k);
                any = true;
            }
        }
    }
    if (any)
        kernAscii_ = std::move(table);
    kerning_ = any;
}

float Font::kernSlow(uint32_t left, uint32_t right) const
{
    const uint64_t key = (static_cast<uint64_t>(left) << 32) | right;
    auto it = kernCache_.find(key);
    if (it == kernCache_.end()) {
        const int k = stbtt_GetGlyphKernAdvance(&info_, glyphIndex(left), glyphIndex(right));
        it = kernCache_.emplace(key, static_cast<int16_t>(k)).first;
    }
    return it->second * scale_;
}

void Font::setPixelSize(float px)
{
    pixelSize_ = px;
    scale_ = stbtt_ScaleForMappingEmToPixels(&info_, px);

    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&info_, &ascent, &descent, &lineGap);
    ascent_ = static_cast<int>(std::ceil(ascent * scale_));
    lineHeight_ = static_cast<int>(std::ceil((ascent - descent + lineGap) * scale_));

    // advanceWidthMax: big-endian uint16 at offset 10 of the hhea table.
    const unsigned char* hhea = info_.data + info_.hhea;
    maxAdvance_ = static_cast<float>(((hhea[10] << 8) | hhea[11]) + maxKern_) * scale_;

    for (uint32_t cp = 0; cp < asciiAdvance_.size(); ++cp)
        asciiAdvance_[cp] = advance(glyphIndex(cp));
    advanceCache_.clear();
}

int Font::glyphIndex(uint32_t codepoint) const
{
    return stbtt_FindGlyphIndex(&info_, static_cast<int>(codepoint));
}

float Font::advance(int glyph) const
{
    int advanceWidth = 0, leftBearing = 0;
    stbtt_GetGlyphHMetrics(&info_, glyph, &advanceWidth, &leftBearing);
    return advanceWidth * scale_;
}

float Font::advanceOf(uint32_t codepoint) const
{
    if (codepoint < asciiAdvance_.size())
        return asciiAdvance_[codepoint];
    if (auto it = advanceCache_.find(codepoint); it != advanceCache_.end())
        return it->second;
    const float adv = advance(glyphIndex(codepoint));
    advanceCache_.emplace(codepoint, adv);
    return adv;
}
