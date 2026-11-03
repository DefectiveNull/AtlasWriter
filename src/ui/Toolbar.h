#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <vector>

class TextRenderer;

// A row of buttons and labels across the top of a tile (reader, web and
// audio tiles). Items are laid out left to right, the ones marked `right`
// from the right edge, and one `flex` item takes the room left between.
class Toolbar
{
public:
    enum class Icon { None, Back, Forward, Reload, Play, Pause, Minus, Plus };

    struct Item
    {
        int id = 0;        // 0: a label, not clickable
        std::string text;  // drawn after the icon, if any
        Icon icon = Icon::None;
        bool enabled = true;
        bool right = false; // placed from the right edge
        bool flex = false;  // takes the remaining width (text is cut to fit)
        bool boxed = false; // drawn with a field's background (an address)
    };

    // The bar's height for a UI font.
    static float heightFor(const TextRenderer& text);

    // Lays out `items` across `bar`.
    void set(const SDL_FRect& bar, const TextRenderer& text, std::vector<Item> items);
    const SDL_FRect& bounds() const { return bar_; }

    // The id of the enabled button under the point, or 0.
    int hit(float x, float y) const;
    // Hover highlight; returns true if it changed.
    bool hover(float x, float y);
    SDL_FRect rectOf(int id) const;

    void draw(SDL_Renderer* renderer, TextRenderer& text) const;

private:
    std::vector<Item> items_;
    std::vector<SDL_FRect> rects_;
    SDL_FRect bar_{};
    int hovered_ = 0;
};

namespace ui {

// Small vector icons, centred on (cx, cy), `size` pixels across.
void drawIcon(SDL_Renderer* renderer, Toolbar::Icon icon, float cx, float cy, float size, SDL_FColor color);

// Lines of muted text centred in `area` (empty states and errors). The
// first line is drawn brighter.
void drawMessage(SDL_Renderer* renderer, TextRenderer& text, const SDL_FRect& area,
                 const std::vector<std::string>& lines);

} // namespace ui
