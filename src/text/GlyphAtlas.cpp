#include "GlyphAtlas.h"

#include "Font.h"

#include <algorithm>

namespace {
constexpr int kPadding = 1; // keeps neighbouring glyphs from bleeding into each other
}

GlyphAtlas::GlyphAtlas(SDL_Renderer* renderer, int size)
    : size_(size)
    , pixels_(static_cast<size_t>(size) * size, 0)
{
    texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STATIC, size, size);
    if (!texture_) {
        SDL_Log("Failed to create glyph atlas: %s", SDL_GetError());
        return;
    }
    SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND);
    // Glyphs are drawn at exactly 1:1 on whole pixels, so filtering would only blur.
    SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);
    reset();
}

GlyphAtlas::~GlyphAtlas()
{
    SDL_DestroyTexture(texture_);
}

int GlyphAtlas::addFace(const Font& font)
{
    faces_.emplace_back().font = &font;
    return static_cast<int>(faces_.size() - 1);
}

const Glyph* GlyphAtlas::get(int face, uint32_t codepoint)
{
    Face& f = faces_[static_cast<size_t>(face)];
    if (codepoint < 128) {
        Glyph& slot = f.ascii[codepoint];
        if (!f.asciiValid[codepoint]) {
            if (!rasterize(*f.font, codepoint, slot))
                return nullptr;
            f.asciiValid[codepoint] = true;
        }
        return &slot;
    }

    const uint64_t key = (static_cast<uint64_t>(face) << 32) | codepoint;
    if (auto it = other_.find(key); it != other_.end())
        return &it->second;

    Glyph glyph;
    if (!rasterize(*f.font, codepoint, glyph))
        return nullptr;
    return &other_.emplace(key, glyph).first->second;
}

size_t GlyphAtlas::glyphCount() const
{
    size_t count = other_.size();
    for (const Face& face : faces_)
        count += face.asciiValid.count();
    return count;
}

void GlyphAtlas::beginFrame()
{
    if (overflowed_) {
        SDL_Log("Glyph atlas full (%zu glyphs), clearing", glyphCount());
        reset();
    }
}

void GlyphAtlas::reset()
{
    for (Face& face : faces_)
        face.asciiValid.reset();
    other_.clear();
    penX_ = kPadding;
    penY_ = kPadding;
    rowHeight_ = 0;
    overflowed_ = false;

    // Clear stale pixels too, so padding around new glyphs is truly empty.
    std::fill(pixels_.begin(), pixels_.end(), 0u);
    markDirty(0, 0, size_, size_);
}

void GlyphAtlas::upload()
{
    if (dirty_.w == 0 || !texture_)
        return;
    const uint32_t* origin = pixels_.data() + static_cast<size_t>(dirty_.y) * size_ + dirty_.x;
    SDL_UpdateTexture(texture_, &dirty_, origin, size_ * static_cast<int>(sizeof(uint32_t)));
    dirty_ = {0, 0, 0, 0};
}

bool GlyphAtlas::rasterize(const Font& font, uint32_t codepoint, Glyph& out)
{
    const stbtt_fontinfo& info = font.info();
    const float scale = font.scale();
    const int glyph = font.glyphIndex(codepoint); // 0 = .notdef box when missing

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&info, glyph, scale, scale, &x0, &y0, &x1, &y1);
    const int w = x1 - x0;
    const int h = y1 - y0;

    out = {};
    out.advance = font.advance(glyph);
    out.xoff = static_cast<int16_t>(x0);
    out.yoff = static_cast<int16_t>(y0);

    if (w <= 0 || h <= 0)
        return true; // whitespace: advance only, nothing to draw

    int ax = 0, ay = 0;
    if (!pack(w, h, ax, ay)) {
        overflowed_ = true;
        return false;
    }

    scratch_.resize(static_cast<size_t>(w) * h);
    stbtt_MakeGlyphBitmap(&info, scratch_.data(), w, h, w, scale, scale, glyph);

    // Store coverage in alpha over white; text color comes from vertex colors.
    for (int row = 0; row < h; ++row) {
        const unsigned char* src = scratch_.data() + static_cast<size_t>(row) * w;
        uint32_t* dst = pixels_.data() + static_cast<size_t>(ay + row) * size_ + ax;
        for (int col = 0; col < w; ++col)
            dst[col] = (static_cast<uint32_t>(src[col]) << 24) | 0x00FFFFFFu;
    }
    markDirty(ax, ay, w, h);

    out.x = static_cast<uint16_t>(ax);
    out.y = static_cast<uint16_t>(ay);
    out.w = static_cast<uint16_t>(w);
    out.h = static_cast<uint16_t>(h);
    return true;
}

bool GlyphAtlas::pack(int w, int h, int& x, int& y)
{
    if (w + 2 * kPadding > size_)
        return false;

    if (penX_ + w + kPadding > size_) { // start a new shelf
        penX_ = kPadding;
        penY_ += rowHeight_;
        rowHeight_ = 0;
    }
    if (penY_ + h + kPadding > size_)
        return false;

    x = penX_;
    y = penY_;
    penX_ += w + kPadding;
    rowHeight_ = std::max(rowHeight_, h + kPadding);
    return true;
}

void GlyphAtlas::markDirty(int x, int y, int w, int h)
{
    if (dirty_.w == 0) {
        dirty_ = {x, y, w, h};
        return;
    }
    const int x0 = std::min(dirty_.x, x);
    const int y0 = std::min(dirty_.y, y);
    const int x1 = std::max(dirty_.x + dirty_.w, x + w);
    const int y1 = std::max(dirty_.y + dirty_.h, y + h);
    dirty_ = {x0, y0, x1 - x0, y1 - y0};
}
