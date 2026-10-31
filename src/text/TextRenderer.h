#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

class Font;
class GlyphAtlas;

// Collects glyph quads from every TextRenderer that shares it and submits
// them as a single SDL_RenderGeometry call on flush(). All fonts live in the
// same atlas texture and color is per vertex, so mixed fonts and colors
// still cost one draw call.
//
// Text is drawn at flush time, so call flush() before drawing anything that
// must appear on top of the text queued so far.
class TextBatch
{
public:
    TextBatch(SDL_Renderer* renderer, GlyphAtlas& atlas);

    void flush();
    size_t quadsLastFlush() const { return quadsLastFlush_; }

private:
    friend class TextRenderer;

    SDL_Renderer* renderer_;
    GlyphAtlas& atlas_;
    std::vector<SDL_Vertex> vertices_;
    std::vector<int> indices_; // fixed quad pattern, only ever grows
    size_t quadsLastFlush_ = 0;
};

// Lays out and queues text in one font. Cheap to have one per font/size.
//
// All layout functions work on a single line and expand tabs to tabSize()
// spaces, measured from the start of the string passed in.
class TextRenderer
{
public:
    int tabSize() const { return tabSize_; }
    void setTabSize(int spaces) { tabSize_ = spaces < 1 ? 1 : spaces; }

    TextRenderer(TextBatch& batch, GlyphAtlas& atlas, const Font& font);

    const Font& font() const { return font_; }

    // Queues one line of UTF-8 text with its top-left at (x, y). Glyphs left of
    // clipLeft are skipped and layout stops past clipRight, so very long lines
    // only cost what is visible.
    void draw(float x, float y, std::string_view utf8, SDL_FColor color,
              float clipLeft = std::numeric_limits<float>::lowest(),
              float clipRight = std::numeric_limits<float>::max());

    // Like draw(), but colored in runs: runs[i].color applies from byte
    // runs[i].start up to the next run's start. Runs must be sorted; text
    // before the first run uses `color`. One pass, so tab stops stay right.
    struct ColorRun
    {
        size_t start;
        SDL_FColor color;
    };
    void draw(float x, float y, std::string_view utf8, SDL_FColor color, const ColorRun* runs, size_t runCount,
              float clipLeft = std::numeric_limits<float>::lowest(),
              float clipRight = std::numeric_limits<float>::max());

    void flush() { batch_.flush(); } // flushes the shared batch

    float advance(uint32_t codepoint) const;

    // Width of the first `bytes` bytes of a line.
    float measure(std::string_view utf8, size_t bytes = std::string_view::npos) const;

    // Byte offset of the caret position closest to `x` within a line.
    size_t hitTest(std::string_view utf8, float x) const;

    // Length in bytes of the first row when wrapping `utf8` to `width`.
    // Prefers breaking after whitespace (which may hang past the edge), falls
    // back to breaking between codepoints, and always takes at least one
    // codepoint so wrapping makes progress. Returns utf8.size() if it fits.
    size_t fit(std::string_view utf8, float width) const;

private:
    template <typename Fn>
    void walk(std::string_view utf8, Fn&& fn) const;

    TextBatch& batch_;
    GlyphAtlas& atlas_;
    const Font& font_;
    int face_;
    int tabSize_ = 4;
};
