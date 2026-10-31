#pragma once

#include "markdown/Markdown.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class ImageCache;
class TextRenderer;

// The fonts a rendered Markdown document uses.
struct MarkdownFonts
{
    TextRenderer* regular = nullptr;
    TextRenderer* bold = nullptr;
    TextRenderer* italic = nullptr;
    TextRenderer* boldItalic = nullptr;
    TextRenderer* code = nullptr;
    TextRenderer* heading[6] = {}; // h1..h6
};

// Read-only rendered view of a Markdown document.
//
// The source is parsed once per change and laid out once per width or font
// change into positioned text runs, images and boxes; drawing then only
// walks the lines that intersect the viewport.
//
// Headings can be collapsed (click them): everything up to the next heading
// of the same or a higher level is hidden. Collapsed state is keyed by the
// heading's level, text and occurrence, so it survives edits elsewhere.
class MarkdownView
{
public:
    void setFonts(const MarkdownFonts& fonts);
    void invalidateLayout() { layoutWidth_ = -1.0f; }

    // Images are loaded through `images`, relative paths resolved against
    // `baseDirectory` (the document's folder, with a trailing separator).
    // `scale` converts image pixels to screen pixels (the display scale).
    void setImages(ImageCache* images, std::string baseDirectory, float scale);

    // Re-parses only when `version` differs from the last call.
    void setSource(std::string_view text, uint64_t version);

    void setBounds(const SDL_FRect& bounds) { bounds_ = bounds; }

    // Returns true if the view needs a redraw.
    bool handleEvent(const SDL_Event& event);
    void draw(SDL_Renderer* renderer);

    // Scrolls so the block containing source line `line` is at the top.
    void scrollToSourceLine(size_t line);

    // Source line of the first thing in view (for the outline's highlight).
    size_t topSourceLine();
    // Scrolls to a source line, expanding any collapsed heading hiding it.
    void revealSourceLine(size_t line);

    bool isOverHeading(float x, float y) const;
    void collapseAll();
    void expandAll();

private:
    enum Decoration : uint8_t { kUnderline = 1, kStrikeThrough = 2 };

    struct Run
    {
        TextRenderer* text;
        float x;
        float width;
        std::string str;
        SDL_FColor color;
        uint8_t decoration;
        bool codeBackground;
        SDL_Texture* image = nullptr; // an image instead of text, sitting on the baseline
        float imageHeight = 0.0f;
    };

    struct Line
    {
        float y = 0.0f;
        float height = 0.0f;
        float baseline = 0.0f; // from y
        size_t sourceLine = 0;
        std::vector<Run> runs;
    };

    struct Box
    {
        SDL_FRect rect;
        SDL_FColor color;
    };

    // Clickable heading, in document coordinates.
    struct HeadingHit
    {
        float top, bottom;
        float markerY; // vertical center of the disclosure triangle
        size_t block;
    };

    void layout();
    void layoutBlock(const md::Block& block, const md::Block* next, float& y);
    void flow(const md::Block& block, float left, float right, float& y, float spacing);
    void finishLine(Line& line, float& y, float spacing, TextRenderer* base);
    void layoutTable(const md::Block& table, float left, float right, float& y);
    void measureCell(const md::Block& cell, float& natural, float& minimum);
    TextRenderer* fontFor(const md::Block& block, uint8_t style) const;
    bool imageSize(const md::Span& span, float maxWidth, SDL_Texture*& texture, float& w, float& h);
    std::string resolveImage(const std::string& url) const;

    void buildHeadingKeys();
    int headingAt(float x, float y) const; // index into headings_, or -1
    void toggle(size_t block);
    void drawMarker(SDL_Renderer* renderer, float x, float y, bool collapsed, SDL_FColor color) const;

    float bodyLineHeight() const;
    float em() const;
    float columnWidth() const;
    float columnLeft() const;
    void clampScroll();
    void ensureLayout();

    MarkdownFonts fonts_;
    SDL_FRect bounds_{};

    ImageCache* images_ = nullptr;
    std::string baseDirectory_;
    float imageScale_ = 1.0f;

    std::vector<md::Block> blocks_;
    uint64_t version_ = UINT64_MAX;

    std::vector<std::string> headingKeys_; // per block; empty for non-headings
    std::unordered_set<std::string> collapsed_;
    std::vector<HeadingHit> headings_;     // visible headings, sorted by top
    int hoverHeading_ = -1;

    std::vector<Line> lines_; // sorted by y
    std::vector<Box> boxes_;
    float contentHeight_ = 0.0f;
    float maxLineHeight_ = 0.0f; // tallest line, bounds the visibility search
    float layoutWidth_ = -1.0f;
    float scrollY_ = 0.0f;
};
