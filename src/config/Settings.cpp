#include "Settings.h"

#include <algorithm>
#include <charconv>

namespace {

constexpr const char* kPreferences = R"(/= AtlasWriter settings, in CCL (the Categorical Configuration Language):
/= https://chshersh.com/blog/2025-01-06-the-most-elegant-configuration-language.html
/=
/= Every line is "key = value". Lines indented under a key belong to it.
/= Entries whose key is "/" are comments, like these lines.
/= Save this file (in AtlasWriter or anywhere else) to apply your changes;
/= anything you delete falls back to its default.

font =
  /= Size in points. Zoom in/out only changes it for the current session.
  size = 15
  /= Reading font (text and Markdown) size relative to the code font.
  reading scale = 1.1

editor =
  /= Tab stops, in spaces.
  tab size = 4
  /= Caret blink interval in milliseconds; 0 keeps it steady.
  caret blink = 530

code =
  line numbers = true
  highlight current line = true

prose =
  /= Width of the centered text column, in ems; 0 uses the whole window.
  column width = 40

terminal =
  /= Command to run; empty means PowerShell on Windows and $SHELL elsewhere.
  shell =

shortcuts =
  /= The key "Mod" means in the keys below: auto (Ctrl, or Cmd on macOS),
  /= ctrl, shift or super (the Windows key). With shift, shortcuts take over
  /= typing those capitals, and Mod+Shift+S is the same shortcut as Mod+S, so
  /= only the first one listed works. With super, the system keeps some
  /= combinations for itself, like Win+E or Win+L on Windows.
  mod = auto

updates =
  /= Look for a new version on GitHub each time AtlasWriter starts. It is
  /= downloaded in the background and applied when you close AtlasWriter.
  check = true
)";

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

const ccl::Config* lookup(const ccl::Config& config, std::string_view section, std::string_view key)
{
    const ccl::Config* s = config.find(section);
    return s ? s->find(key) : nullptr;
}

// The user's value if present, else the default's.
std::string_view scalar(const ccl::Config& user, const ccl::Config& defaults, std::string_view section,
                        std::string_view key)
{
    if (const ccl::Config* v = lookup(user, section, key))
        return v->value();
    const ccl::Config* d = lookup(defaults, section, key);
    return d ? d->value() : std::string_view();
}

void readFloat(std::string_view text, float lo, float hi, float& out, const std::string& name,
               std::vector<std::string>& warnings)
{
    float value = 0.0f;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size() || value < lo || value > hi) {
        warnings.push_back(name + ": expected a number from " + std::to_string(lo).substr(0, 4) + " to "
                           + std::to_string(hi).substr(0, 4) + ", got \"" + std::string(text) + "\"");
        return;
    }
    out = value;
}

void readInt(std::string_view text, int lo, int hi, int& out, const std::string& name,
             std::vector<std::string>& warnings)
{
    int value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size() || value < lo || value > hi) {
        warnings.push_back(name + ": expected a whole number from " + std::to_string(lo) + " to " + std::to_string(hi)
                           + ", got \"" + std::string(text) + "\"");
        return;
    }
    out = value;
}

void readBool(std::string_view text, bool& out, const std::string& name, std::vector<std::string>& warnings)
{
    if (text == "true" || text == "yes" || text == "on")
        out = true;
    else if (text == "false" || text == "no" || text == "off")
        out = false;
    else
        warnings.push_back(name + ": expected true or false, got \"" + std::string(text) + "\"");
}

} // namespace

std::string Settings::defaultText()
{
    std::string text = kPreferences;
    text += "\nkeys =\n"
            "  /= command = shortcut. Repeat a command to give it more than one\n"
            "  /= shortcut, or leave it empty (\"save =\") to unbind it. Modifiers are\n"
            "  /= Mod (see shortcuts above), Ctrl, Shift, Alt and Cmd; keys are names\n"
            "  /= like F5, Enter, PageUp, Plus, ` or \"Keypad +\".\n";
    for (const CommandInfo& command : kCommands) {
        std::string_view defaults = command.defaults;
        if (defaults.empty()) {
            text += "  " + std::string(command.name) + " =\n";
            continue;
        }
        while (!defaults.empty()) {
            const size_t bar = std::min(defaults.find('|'), defaults.size());
            text += "  " + std::string(command.name) + " = " + std::string(defaults.substr(0, bar)) + "\n";
            defaults.remove_prefix(std::min(bar + 1, defaults.size()));
        }
    }
    return text;
}

std::string Settings::exampleText()
{
    return "/= EXAMPLE ONLY: AtlasWriter doesn't read this file. It shows the defaults;\n"
           "/= your own settings are in Documents/AtlasWriter/settings.ccl\n"
           "/= (File > Preferences opens it).\n"
           "\n"
           + defaultText();
}

bool Settings::load(std::string_view text, Settings& out, std::vector<std::string>& warnings, ccl::Error& error)
{
    ccl::Config defaults;
    ccl::Error ignored;
    ccl::Config::parse(defaultText(), defaults, &ignored);
    ccl::Config user;
    if (!ccl::Config::parse(text, user, &error))
        return false;

    // Anything the defaults don't have is a typo or a setting that doesn't exist.
    for (const auto& [section, entries] : user.children()) {
        const ccl::Config* known = defaults.find(section);
        if (!known) {
            warnings.push_back("unknown section \"" + section + "\"");
            continue;
        }
        if (section == "keys")
            continue; // checked against the command list below
        for (const auto& [key, value] : entries.children()) {
            if (!known->find(key))
                warnings.push_back("unknown setting \"" + section + " / " + key + "\"");
        }
    }

    Settings s;
    readFloat(scalar(user, defaults, "font", "size"), 6.0f, 72.0f, s.fontSize, "font / size", warnings);
    readFloat(scalar(user, defaults, "font", "reading scale"), 0.5f, 3.0f, s.readingScale, "font / reading scale",
              warnings);
    readInt(scalar(user, defaults, "editor", "tab size"), 1, 16, s.tabSize, "editor / tab size", warnings);
    readInt(scalar(user, defaults, "editor", "caret blink"), 0, 5000, s.caretBlinkMs, "editor / caret blink", warnings);
    readBool(scalar(user, defaults, "code", "line numbers"), s.codeLineNumbers, "code / line numbers", warnings);
    readBool(scalar(user, defaults, "code", "highlight current line"), s.codeHighlightLine,
             "code / highlight current line", warnings);
    readFloat(scalar(user, defaults, "prose", "column width"), 0.0f, 1000.0f, s.proseColumnEms, "prose / column width",
              warnings);
    s.terminalShell = std::string(trim(scalar(user, defaults, "terminal", "shell")));
    readBool(scalar(user, defaults, "updates", "check"), s.updateCheck, "updates / check", warnings);
    const std::string_view modKey = scalar(user, defaults, "shortcuts", "mod");
    if (!Shortcut::parseModKey(modKey, s.modKey))
        warnings.push_back("shortcuts / mod: expected auto, ctrl, shift or super, got \"" + std::string(modKey) + "\"");

    // Shortcuts: a command the user lists replaces all of its defaults.
    const ccl::Config* userKeys = user.find("keys");
    const ccl::Config* defaultKeys = defaults.find("keys");
    if (userKeys) {
        for (const auto& [name, value] : userKeys->children()) {
            const bool known = std::any_of(std::begin(kCommands), std::end(kCommands),
                                           [&](const CommandInfo& c) { return name == c.name; });
            if (!known)
                warnings.push_back("keys: unknown command \"" + name + "\"");
        }
    }
    for (const CommandInfo& command : kCommands) {
        const ccl::Config* chosen = userKeys ? userKeys->find(command.name) : nullptr;
        if (!chosen && defaultKeys)
            chosen = defaultKeys->find(command.name);
        if (!chosen)
            continue;
        for (const auto& [shortcutText, unused] : chosen->children()) {
            Shortcut shortcut;
            if (Shortcut::parse(shortcutText, s.modKey, shortcut))
                s.bindings.push_back({command.id, shortcut});
            else
                warnings.push_back("keys / " + std::string(command.name) + ": can't read shortcut \"" + shortcutText
                                   + "\"");
        }
    }

    out = std::move(s);
    return true;
}

Command Settings::commandFor(const SDL_KeyboardEvent& key) const
{
    for (const Binding& binding : bindings) {
        if (binding.shortcut.matches(key))
            return binding.command;
    }
    return kCmdNone;
}

std::string Settings::shortcutLabel(Command command) const
{
    for (const Binding& binding : bindings) {
        if (binding.command == command)
            return binding.shortcut.toString();
    }
    return {};
}
