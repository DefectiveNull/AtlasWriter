#pragma once

#include <SDL3/SDL.h>

// Platform conventions for shortcut modifiers: Cmd/Option on macOS,
// Ctrl elsewhere.
namespace keys {

#if defined(SDL_PLATFORM_APPLE)
inline bool primary(SDL_Keymod mod) { return (mod & SDL_KMOD_GUI) != 0; }
inline bool word(SDL_Keymod mod) { return (mod & SDL_KMOD_ALT) != 0; }
#else
// AltGr arrives as Ctrl+Alt on Windows; excluding Alt keeps AltGr+Q (`@` on
// many layouts) from triggering Ctrl+Q.
inline bool primary(SDL_Keymod mod) { return (mod & SDL_KMOD_CTRL) != 0 && (mod & SDL_KMOD_ALT) == 0; }
inline bool word(SDL_Keymod mod) { return (mod & SDL_KMOD_CTRL) != 0; }
#endif

// Keycodes for letters can arrive shifted; fold them to the SDLK_* constants.
inline SDL_Keycode normalized(SDL_Keycode key)
{
    return (key >= 'A' && key <= 'Z') ? key + ('a' - 'A') : key;
}

} // namespace keys
