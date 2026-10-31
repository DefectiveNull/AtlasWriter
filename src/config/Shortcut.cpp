#include "Shortcut.h"

#include <algorithm>
#include <cctype>

namespace {

struct KeyName
{
    const char* name;
    SDL_Keycode key;
};

// Friendly names first; the first match for a key is also how it's shown.
constexpr KeyName kKeyNames[] = {
    {"Enter", SDLK_RETURN}, {"Return", SDLK_RETURN}, {"Tab", SDLK_TAB}, {"Space", SDLK_SPACE},
    {"Backspace", SDLK_BACKSPACE}, {"Delete", SDLK_DELETE}, {"Del", SDLK_DELETE}, {"Insert", SDLK_INSERT},
    {"Home", SDLK_HOME}, {"End", SDLK_END}, {"PageUp", SDLK_PAGEUP}, {"PageDown", SDLK_PAGEDOWN},
    {"Up", SDLK_UP}, {"Down", SDLK_DOWN}, {"Left", SDLK_LEFT}, {"Right", SDLK_RIGHT},
    {"Escape", SDLK_ESCAPE}, {"Esc", SDLK_ESCAPE}, {"Plus", SDLK_PLUS}, {"Minus", SDLK_MINUS},
    {"`", SDLK_GRAVE}, {"-", SDLK_MINUS}, {"=", SDLK_EQUALS}, {",", SDLK_COMMA}, {".", SDLK_PERIOD},
    {"/", SDLK_SLASH}, {";", SDLK_SEMICOLON}, {"'", SDLK_APOSTROPHE}, {"[", SDLK_LEFTBRACKET},
    {"]", SDLK_RIGHTBRACKET}, {"\\", SDLK_BACKSLASH}, {"+", SDLK_PLUS},
};

bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

SDL_Keycode keyFromName(std::string_view name)
{
    for (const KeyName& k : kKeyNames) {
        if (equalsIgnoreCase(name, k.name))
            return k.key;
    }
    if (name.size() == 1) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            return static_cast<SDL_Keycode>(c);
    }
    if ((name[0] == 'F' || name[0] == 'f') && name.size() <= 3) { // F1-F24
        int n = 0;
        for (size_t i = 1; i < name.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(name[i])))
                return SDLK_UNKNOWN;
            n = n * 10 + (name[i] - '0');
        }
        if (n >= 1 && n <= 12)
            return SDLK_F1 + static_cast<SDL_Keycode>(n - 1);
        if (n >= 13 && n <= 24)
            return SDLK_F13 + static_cast<SDL_Keycode>(n - 13);
        return SDLK_UNKNOWN;
    }
    return SDL_GetKeyFromName(std::string(name).c_str()); // "Keypad +", "Pause", ...
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

} // namespace

bool Shortcut::parseModKey(std::string_view text, ModKey& out)
{
    text = trim(text);
    if (equalsIgnoreCase(text, "auto"))
        out = kDefaultModKey;
    else if (equalsIgnoreCase(text, "ctrl") || equalsIgnoreCase(text, "control"))
        out = ModKey::Ctrl;
    else if (equalsIgnoreCase(text, "shift"))
        out = ModKey::Shift;
    else if (equalsIgnoreCase(text, "super") || equalsIgnoreCase(text, "win") || equalsIgnoreCase(text, "windows")
             || equalsIgnoreCase(text, "cmd") || equalsIgnoreCase(text, "command") || equalsIgnoreCase(text, "meta"))
        out = ModKey::Super;
    else
        return false;
    return true;
}

bool Shortcut::parse(std::string_view text, ModKey modKey, Shortcut& out)
{
    out = Shortcut{};
    text = trim(text);
    if (text.empty())
        return false;
    // The key is after the last '+', except that "Ctrl++" means the + key.
    size_t split = text.rfind('+');
    if (split == text.size() - 1)
        split = text.size() > 1 ? text.rfind('+', text.size() - 2) : std::string_view::npos;
    const std::string_view keyName = trim(split == std::string_view::npos ? text : text.substr(split + 1));
    std::string_view mods = split == std::string_view::npos ? std::string_view() : text.substr(0, split);

    while (!mods.empty()) {
        const size_t plus = std::min(mods.find('+'), mods.size());
        const std::string_view mod = trim(mods.substr(0, plus));
        mods.remove_prefix(std::min(plus + 1, mods.size()));
        if (equalsIgnoreCase(mod, "ctrl") || equalsIgnoreCase(mod, "control")) {
            out.ctrl = true;
        } else if (equalsIgnoreCase(mod, "shift")) {
            out.shift = true;
        } else if (equalsIgnoreCase(mod, "alt") || equalsIgnoreCase(mod, "option")) {
            out.alt = true;
        } else if (equalsIgnoreCase(mod, "cmd") || equalsIgnoreCase(mod, "command") || equalsIgnoreCase(mod, "super")
                   || equalsIgnoreCase(mod, "win") || equalsIgnoreCase(mod, "meta")) {
            out.gui = true;
        } else if (equalsIgnoreCase(mod, "mod") || equalsIgnoreCase(mod, "primary")) {
            out.ctrl |= modKey == ModKey::Ctrl;
            out.shift |= modKey == ModKey::Shift;
            out.gui |= modKey == ModKey::Super;
        } else {
            return false;
        }
    }
    if (keyName.empty())
        return false;
    out.key = keyFromName(keyName);
    return out.key != SDLK_UNKNOWN;
}

std::string Shortcut::toString() const
{
    std::string s;
#if defined(SDL_PLATFORM_APPLE)
    if (ctrl)
        s += "Ctrl+";
    if (alt)
        s += "Option+";
    if (shift)
        s += "Shift+";
    if (gui)
        s += "Cmd+";
#else
    if (ctrl)
        s += "Ctrl+";
    if (shift)
        s += "Shift+";
    if (alt)
        s += "Alt+";
    if (gui)
        s += "Win+";
#endif
    for (const KeyName& k : kKeyNames) {
        if (k.key == key) {
            s += k.name;
            return s;
        }
    }
    if (key >= 'a' && key <= 'z')
        s += static_cast<char>(key - 'a' + 'A');
    else
        s += SDL_GetKeyName(key);
    return s;
}

bool Shortcut::matches(const SDL_KeyboardEvent& event) const
{
    SDL_Keycode k = event.key;
    if (k >= 'A' && k <= 'Z')
        k += 'a' - 'A';
    return k == key && ((event.mod & SDL_KMOD_CTRL) != 0) == ctrl && ((event.mod & SDL_KMOD_SHIFT) != 0) == shift
        && ((event.mod & SDL_KMOD_ALT) != 0) == alt && ((event.mod & SDL_KMOD_GUI) != 0) == gui;
}
