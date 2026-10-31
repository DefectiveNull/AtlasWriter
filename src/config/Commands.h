#pragma once

// Everything a menu item or shortcut can do. The names are what the
// settings file's `keys` section uses.
enum Command : int {
    kCmdNone = 0,
    kCmdNew,
    kCmdOpen,
    kCmdSave,
    kCmdSaveAs,
    kCmdPreferences,
    kCmdExit,
    kCmdUndo,
    kCmdRedo,
    kCmdCut,
    kCmdCopy,
    kCmdPaste,
    kCmdSelectAll,
    kCmdTogglePreview,
    kCmdToggleOutline,
    kCmdCollapseAll,
    kCmdExpandAll,
    kCmdToggleTerminal,
    kCmdZoomIn,
    kCmdZoomOut,
    kCmdZoomReset,
    kCmdGlyphAtlas,
};

struct CommandInfo
{
    Command id;
    const char* name;     // key in the settings file
    const char* defaults; // default shortcuts, '|'-separated ("Mod" = Ctrl, or Cmd on macOS)
};

inline constexpr CommandInfo kCommands[] = {
    {kCmdNew, "new", "Mod+N"},
    {kCmdOpen, "open", "Mod+O"},
    {kCmdSave, "save", "Mod+S"},
    {kCmdSaveAs, "save as", "Mod+Shift+S"},
    {kCmdPreferences, "preferences", "Mod+,"},
    {kCmdExit, "exit", "Mod+Q"},
    {kCmdUndo, "undo", "Mod+Z"},
#if defined(__APPLE__)
    {kCmdRedo, "redo", "Mod+Shift+Z"},
#else
    {kCmdRedo, "redo", "Mod+Y|Mod+Shift+Z"},
#endif
    {kCmdCut, "cut", "Mod+X"},
    {kCmdCopy, "copy", "Mod+C"},
    {kCmdPaste, "paste", "Mod+V"},
    {kCmdSelectAll, "select all", "Mod+A"},
    {kCmdTogglePreview, "rendered markdown", "Mod+E"},
    {kCmdToggleOutline, "outline", "Mod+Shift+O"},
    {kCmdCollapseAll, "collapse all headings", ""},
    {kCmdExpandAll, "expand all headings", ""},
    {kCmdToggleTerminal, "terminal", "Mod+`|Mod+J"},
    {kCmdZoomIn, "zoom in", "Mod+=|Mod+Plus|Mod+Keypad +"},
    {kCmdZoomOut, "zoom out", "Mod+-|Mod+Keypad -"},
    {kCmdZoomReset, "zoom reset", "Mod+0"},
    {kCmdGlyphAtlas, "glyph atlas", "F1"},
};
