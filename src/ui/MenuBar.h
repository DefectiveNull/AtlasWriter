#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <vector>

class Font;
class TextRenderer;

struct MenuItem
{
    std::string label;
    std::string shortcut; // display only; the app handles the actual key
    int command = 0;      // 0 = separator
    bool enabled = true;
};

struct Menu
{
    std::string title;
    std::vector<MenuItem> items;
};

// A top menu bar with drop-down menus. Choosing an item reports its command
// id to the caller instead of running a callback, so the app stays in charge
// of what commands do.
//
// Mouse: click a title to open, hover other titles to switch, click an item.
// Keyboard: Alt+<first letter> opens a menu, arrows navigate, Enter chooses,
// Esc closes.
class MenuBar
{
public:
    MenuBar(const Font& font, TextRenderer& text);

    void addMenu(Menu menu);
    void clear(); // remove every menu, e.g. to rebuild them for another workspace
    void setEnabled(int command, bool enabled);

    float height() const;
    bool isOpen() const { return open_ >= 0; }

    // Returns true if the event was consumed and must not reach the editor.
    // `command` is set when an item was chosen; `redraw` when anything changed.
    bool handleEvent(const SDL_Event& event, int& command, bool& redraw);

    void draw(SDL_Renderer* renderer, float width); // the bar itself
    void drawDropdown(SDL_Renderer* renderer);      // call last, on top of everything

private:
    bool handleKey(const SDL_KeyboardEvent& key, int& command, bool& redraw);
    void open(int index);
    void close();
    void step(int direction);

    float padX() const;
    float padY() const;
    float itemHeight(const MenuItem& item) const;
    SDL_FRect titleRect(int index) const;
    SDL_FRect dropdownRect() const;
    int titleAt(float x, float y) const;
    int itemAt(float x, float y) const;

    const Font& font_;
    TextRenderer& text_;
    std::vector<Menu> menus_;
    int open_ = -1;
    int hoverTitle_ = -1;
    int hoverItem_ = -1;
};
