#pragma once

#include "ui/MenuBar.h" // MenuItem

#include <SDL3/SDL.h>

#include <vector>

class TextRenderer;

// A drop-down list anchored under a button, such as a tile's type menu.
// One item may be checked. Mouse: hover and click; a click outside closes
// it. Keys: Up/Down, Enter chooses, Esc closes.
class PopupMenu
{
public:
    explicit PopupMenu(TextRenderer& text);

    // `anchor` is the button: the list opens below its left edge. With
    // `keyboard`, the checked item starts highlighted.
    void open(const SDL_FRect& anchor, std::vector<MenuItem> items, int checkedCommand, bool keyboard);
    void close();
    bool isOpen() const { return open_; }

    // While open, takes every keyboard and mouse event. Returns true if it
    // needs a redraw; `command` is set when an item was chosen.
    bool handleEvent(const SDL_Event& event, int& command);
    void draw(SDL_Renderer* renderer, float windowWidth, float windowHeight);

private:
    float padX() const;
    float itemHeight(const MenuItem& item) const;
    SDL_FRect rect() const;
    int itemAt(float x, float y) const;
    void step(int direction);

    TextRenderer& text_;
    bool open_ = false;
    SDL_FRect anchor_{};
    std::vector<MenuItem> items_;
    int checked_ = 0;
    int hover_ = -1;
    float windowWidth_ = 0.0f;
    float windowHeight_ = 0.0f;
};
