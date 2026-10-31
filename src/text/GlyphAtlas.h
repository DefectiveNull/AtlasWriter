#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <bitset>
#include <cstdint>
#include <unordered_map>
#include <vector>

class Font;

struct Glyph
{
    uint16_t x = 0, y = 0, w = 0, h = 0; // rect inside the atlas, in pixels
    int16_t xoff = 0, yoff = 0;          // bitmap offset from the pen position on the baseline
    float advance = 0.0f;
};

// Lazily rasterizes glyphs of any number of fonts into one GPU texture,
// packed with a shelf packer. Sharing one texture means all text, whatever
// its font, can go out in a single draw call.
//
// Glyphs are written to a CPU-side copy of the atlas first and uploaded in a
// single SDL_UpdateTexture call per flush (only the dirty region), so caching
// a whole screen of new glyphs costs one upload instead of one per glyph.
class GlyphAtlas
{
public:
    explicit GlyphAtlas(SDL_Renderer* renderer, int size = 1024);
    ~GlyphAtlas();

    GlyphAtlas(const GlyphAtlas&) = delete;
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;

    // Registers a font and returns its face id for get(). The font must
    // outlive the atlas; its size may change as long as reset() follows.
    int addFace(const Font& font);

    // Returns nullptr only if the atlas is full; it is cleared next frame.
    // Pointers stay valid until reset() / beginFrame() clears the cache.
    const Glyph* get(int face, uint32_t codepoint);

    void beginFrame();
    void reset(); // drop every glyph, e.g. after a font size change
    void upload();

    SDL_Texture* texture() const { return texture_; }
    int size() const { return size_; }
    size_t glyphCount() const;

private:
    struct Face
    {
        const Font* font = nullptr;
        // Fast path for ASCII; everything else goes through other_.
        std::array<Glyph, 128> ascii{};
        std::bitset<128> asciiValid;
    };

    bool rasterize(const Font& font, uint32_t codepoint, Glyph& out);
    bool pack(int w, int h, int& x, int& y);
    void markDirty(int x, int y, int w, int h);

    SDL_Texture* texture_ = nullptr;
    int size_;

    std::vector<Face> faces_;
    // Keyed by (face << 32 | codepoint); node-based, so Glyph pointers
    // survive rehashing.
    std::unordered_map<uint64_t, Glyph> other_;

    std::vector<uint32_t> pixels_;       // CPU copy of the atlas (ARGB8888)
    std::vector<unsigned char> scratch_; // one glyph's coverage bitmap
    SDL_Rect dirty_{0, 0, 0, 0};

    int penX_ = 0, penY_ = 0, rowHeight_ = 0;
    bool overflowed_ = false;
};
