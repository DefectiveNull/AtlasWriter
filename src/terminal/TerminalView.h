#pragma once

#include "terminal/Pty.h"
#include "terminal/TerminalScreen.h"
#include "text/TextRenderer.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

// The code workspace's terminal panel: a header strip (title, close button;
// dragging it resizes the panel) above the shell's screen.
//
// The shell starts the first time the panel is shown and keeps running
// while hidden. Keys go to the shell while the panel has focus, except
// Ctrl+Shift+C / Ctrl+Shift+V (copy / paste) and Shift+PageUp/PageDown
// (scrollback).
class TerminalView
{
public:
    TerminalView(TextRenderer& text, Uint32 eventType);

    void setBounds(const SDL_FRect& bounds) { bounds_ = bounds; }
    const SDL_FRect& bounds() const { return bounds_; }
    float headerHeight() const;

    // Starts the shell if it isn't running.
    void ensureStarted(const std::string& directory);
    // Command line for shells started from now on (empty: the default).
    void setShell(std::string shell) { shell_ = std::move(shell); }
    // Call on the pty's event: consumes output. Returns true if it changed.
    bool onOutput();

    // Returns true if the view needs a redraw.
    bool handleEvent(const SDL_Event& event);
    void draw(SDL_Renderer* renderer);

    void setFocused(bool focused) { focused_ = focused; }
    bool focused() const { return focused_; }
    bool contains(float x, float y) const;
    bool isOverHeader(float x, float y) const;
    bool isOverCloseButton(float x, float y) const;

private:
    struct Point
    {
        long long line; // index into scrollback + screen
        int col;
    };

    void layoutGrid();
    bool handleKey(const SDL_KeyboardEvent& key);
    void send(std::string_view bytes);
    void paste();
    void copySelection();
    Point cellAt(float x, float y) const;
    const TerminalScreen::Line* lineAt(long long index) const;
    SDL_FColor color(uint32_t value, bool foreground, bool bold) const;
    SDL_FRect closeButton() const;

    TextRenderer& text_;
    Pty pty_;
    TerminalScreen screen_;
    SDL_FRect bounds_{};
    std::string directory_;
    std::string shell_;
    bool focused_ = false;
    bool exitedShown_ = false;

    float cellW_ = 1.0f, cellH_ = 1.0f;
    size_t viewOffset_ = 0; // lines scrolled back from the bottom

    bool selecting_ = false;
    bool hasSelection_ = false;
    Point selStart_{}, selEnd_{};

    std::string rowText_;
    std::vector<TextRenderer::ColorRun> runs_;
};
