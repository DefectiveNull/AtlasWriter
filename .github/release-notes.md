AtlasWriter 1.1 makes to-do lists usable from the rendered view.

## What's new

**Markdown**
- Click a to-do item's checkbox in the rendered view (Ctrl+E) to tick or untick it. AtlasWriter writes `x` or a space into the file, and the caret in the source stays where it was.
- Undo and redo now work in the rendered view too, so an accidental click can be reverted without switching back to the source.
- Checkboxes written as HTML (`<input type="checkbox">`) still show but can't be clicked.

If you already have 1.0.0, AtlasWriter updates itself the next time you start and close it.

## Install

Download the archive for your system below and extract it. Nothing needs admin or root rights.

**Windows 10 or later**

Open PowerShell in the extracted `AtlasWriter` folder and run:

```
powershell -ExecutionPolicy Bypass -File install.ps1
```

AtlasWriter then appears in the Start menu.

**Linux (x64)**

Open a terminal in the extracted `AtlasWriter` folder and run:

```
sh install.sh
```

AtlasWriter then appears in your app launcher. Automatic updates need `curl`.

## Known limits

- The builds aren't code signed, so Windows SmartScreen may warn you before running them.
- There's no macOS build yet.
- The Linux build has only been tested under WSL with X11, not on a full desktop. Native Wayland, the app launcher entry and the icon are untested.
- With `check = false`, leftover files from a failed update stay in the install folder until checks are turned back on.
