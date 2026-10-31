#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <string_view>

// The key that "Mod" stands for in a shortcut.
enum class ModKey
{
    Ctrl,
    Shift,
    Super, // the Windows key, or Cmd on macOS
};

#if defined(SDL_PLATFORM_APPLE)
inline constexpr ModKey kDefaultModKey = ModKey::Super;
#else
inline constexpr ModKey kDefaultModKey = ModKey::Ctrl;
#endif

// A key plus an exact set of modifiers, written like "Ctrl+Shift+S".
//
// Modifier names: Ctrl/Control, Shift, Alt/Option, Cmd/Command/Super/Win/
// Meta, and Mod (see ModKey). Key names: letters, digits, F1-F24,
// Enter, Tab, Space, Backspace, Delete, Insert, Home, End, PageUp, PageDown,
// Up, Down, Left, Right, Escape, Plus, Minus, the punctuation characters
// themselves (` - = , . / ; ' [ ] \), or any key name SDL knows.
//
// Matching requires exactly these modifiers, so AltGr (which arrives as
// Ctrl+Alt on Windows) never triggers a Ctrl shortcut.
struct Shortcut
{
    SDL_Keycode key = SDLK_UNKNOWN;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool gui = false;

    static bool parse(std::string_view text, ModKey modKey, Shortcut& out);
    // Reads "auto", "ctrl", "shift" or "super" (or an alias of those).
    static bool parseModKey(std::string_view text, ModKey& out);
    std::string toString() const; // for menus, e.g. "Ctrl+Shift+S"
    bool matches(const SDL_KeyboardEvent& event) const;
};
