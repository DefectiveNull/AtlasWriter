#pragma once

#include "config/Ccl.h"
#include "config/Commands.h"
#include "config/Shortcut.h"

#include <SDL3/SDL.h>

#include <string>
#include <string_view>
#include <vector>

// Preferences and shortcuts, read from a CCL file (see Ccl.h).
//
// The built-in defaults are themselves a CCL document (defaultText()); the
// user's file is read on top of it, so any setting it leaves out keeps its
// default. The defaults also act as the schema: keys they don't have are
// reported as unknown.
struct Settings
{
    struct Binding
    {
        Command command;
        Shortcut shortcut;
    };

    float fontSize = 15.0f;    // points
    float readingScale = 1.1f; // reading font relative to the code font
    int tabSize = 4;
    int caretBlinkMs = 530;    // 0 = steady caret
    bool codeLineNumbers = true;
    bool codeHighlightLine = true;
    float proseColumnEms = 40.0f; // 0 = use the full window width
    std::string terminalShell;    // empty = the platform default
    ModKey modKey = kDefaultModKey;
    std::vector<Binding> bindings;

    // The CCL text of the defaults, written out as the initial settings file.
    static std::string defaultText();

    // defaultText() with a note that it's a reference copy, for the example
    // file the build places next to the executable.
    static std::string exampleText();

    // Reads `text` on top of the defaults into `out`. Returns false (leaving
    // `out` alone) on a syntax error; questionable entries are skipped and
    // described in `warnings`.
    static bool load(std::string_view text, Settings& out, std::vector<std::string>& warnings, ccl::Error& error);

    Command commandFor(const SDL_KeyboardEvent& key) const; // kCmdNone if unbound
    std::string shortcutLabel(Command command) const;       // first binding, for menus
};
