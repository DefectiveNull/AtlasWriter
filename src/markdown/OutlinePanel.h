#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class TextBuffer;
class TextRenderer;

struct OutlineEntry
{
    int level;        // 1-6
    std::string text; // inline markup removed
    size_t line;      // 0-based source line
};

// ATX headings ("## Title") outside fenced code blocks. A straight line
// scan over the buffer (no full Markdown parse), cheap enough to rerun
// after edits.
std::vector<OutlineEntry> scanOutline(const TextBuffer& buffer);

// Side panel listing the document's headings. Clicking one reports its
// source line so the app can scroll the editor or the rendered view there;
// the heading whose section contains the current line is highlighted.
class OutlinePanel
{
public:
    explicit OutlinePanel(TextRenderer& text);

    void setBounds(const SDL_FRect& bounds) { bounds_ = bounds; }
    const SDL_FRect& bounds() const { return bounds_; }

    // Rescans if the buffer changed since the last call.
    void update(const TextBuffer& buffer);
    void setCurrentLine(size_t line);

    // Returns true if the panel needs a redraw. On a click on an entry,
    // `jumpTo` is set to that heading's line.
    bool handleEvent(const SDL_Event& event, long long& jumpTo);
    void draw(SDL_Renderer* renderer);

    bool contains(float x, float y) const;

private:
    float itemHeight() const;
    float headerHeight() const;
    int entryAt(float x, float y) const;
    void clampScroll();

    TextRenderer& text_;
    SDL_FRect bounds_{};
    std::vector<OutlineEntry> entries_;
    uint64_t version_ = UINT64_MAX;
    int hover_ = -1;
    int current_ = -1;
    float scrollY_ = 0.0f;
};
