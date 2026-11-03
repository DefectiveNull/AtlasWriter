#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <vector>

class TextRenderer;

// A small box at the top of the window: a line to type in over a list of
// choices that the typing filters. Up/Down pick, Enter chooses, Esc or a
// click outside cancels. With free text on, Enter takes what was typed
// unless a choice was picked with the arrows.
class Picker
{
public:
    explicit Picker(TextRenderer& text);

    void open(int id, std::string prompt, std::vector<std::string> choices, bool freeText);
    void close();
    bool isOpen() const { return open_; }
    int id() const { return id_; }

    // While open, takes every keyboard and mouse event. Returns true if
    // the picker needs a redraw.
    bool handleEvent(const SDL_Event& event);
    // The chosen text, once.
    bool takeResult(std::string& out);

    void draw(SDL_Renderer* renderer, float windowWidth, float top);

private:
    void filter();
    void choose();
    float rowHeight() const;
    SDL_FRect box() const;

    TextRenderer& text_;
    bool open_ = false;
    int id_ = 0;
    bool freeText_ = false;
    std::string prompt_;
    std::string typed_;
    std::vector<std::string> choices_;
    std::vector<size_t> shown_; // indices into choices_ matching typed_
    int selected_ = -1;         // index into shown_
    bool done_ = false;
    std::string result_;
    float windowWidth_ = 0.0f;
    float top_ = 0.0f;
};
