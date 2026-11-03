AtlasWriter 1.2 splits the window into tiles, in the style of the i3 window manager. Each tile shows an editor, a rendered preview, an outline or a terminal, and you can arrange them however you like.

## What's new

**Tiles**
- Split any tile side by side (Alt+\\) or stacked (Alt+-), and close it with Alt+Backspace.
- Alt+arrows move the focus, Alt+Shift+arrows move the focused tile, and Alt+Ctrl+arrows resize it.
- Drag the line between two tiles to resize them.
- The View menu opens or closes an editor, preview, outline or terminal tile for any file:
  - The outline opens on the left.
  - The preview opens beside the editor.
  - The terminal opens across the bottom.
- The Window menu has every tile and tab command.

**Tile headers**
- Every tile has a header.
- The button on its left (`Editor ▾`) changes what the tile shows. Switching back brings back what it had: the editor's tabs and caret, and the terminal's running shells.
- Drag a header onto another tile to move it there:
  - Drop on a tile's edge to split it on that side.
  - Drop in its middle to swap the two tiles.
  - Drop at the window's edge to dock along that whole edge.
  - A shaded area shows where the tile will land. Esc cancels the drag.
- The × on the right closes the tile.

**Tabs**
- An editor tile holds several documents as tabs, and a terminal tile holds several shells.
- With two or more tabs, they show in the header:
  - Click a tab to switch to it, or middle-click to close it.
  - Ctrl+Tab and Ctrl+Shift+Tab move between tabs, and Ctrl+W closes the current one.
- Opening a file that's already open goes to its tab. Otherwise the file opens in the editor you used last.

**The same file in two tiles**
- Splitting an editor shows the same document in both tiles. Each tile has its own caret and scroll position.
- An edit in one appears in the other right away, and undo covers both.

**Layouts**
- Describe layouts in `settings.ccl` under `layouts`, and pick the one the window opens with using `layout = <name>`. For example:

```
layouts =
  research =
    split = horizontal
    outline = 20%
    editor = 50%
    right =
      split = vertical
      preview = 70%
      terminal = 30%
```

- Window > Switch Layout... (Ctrl+Alt+L) changes layout, and your open documents carry over.
- Window > Save Layout As... saves the current arrangement into `settings.ccl`.
- AtlasWriter comes with two layouts: `default` (one editor) and `compare` (two editors side by side).

## Changes from 1.1

- The terminal and the outline are now tiles. Ctrl+J and Ctrl+Shift+O still open and close them.
- While a terminal has the focus, the shell's own keys win: Ctrl+C, Ctrl+V, Ctrl+W, Ctrl+E and the rest go to the shell instead of AtlasWriter.
- When unsaved documents would be lost, by closing a tab or tile, switching layout or exiting, AtlasWriter asks once about all of them.

## Updating from 1.1

AtlasWriter updates itself the next time you start and close it, and your `settings.ccl` keeps working as it is.

If you're coming from 1.1.0, the File Explorer integration from 1.1.1 still needs turning on once. Run this in PowerShell:

```
powershell -ExecutionPolicy Bypass -File "$env:LOCALAPPDATA\Programs\AtlasWriter\explorer.ps1" -Register
```

## Install

Download the archive for your system below and extract it. AtlasWriter installs for your user only; on Windows, the File Explorer step asks for admin rights once.

**Windows 10 or later**

Open PowerShell in the extracted `AtlasWriter` folder and run:

```
powershell -ExecutionPolicy Bypass -File install.ps1
```

AtlasWriter then appears in the Start menu. Approve the Windows prompt (UAC) to show markdown metadata in File Explorer, or decline it to skip that part.

**Linux (x64)**

Open a terminal in the extracted `AtlasWriter` folder and run:

```
sh install.sh
```

AtlasWriter then appears in your app launcher. Automatic updates need `curl`.

## Known limits

- You can't drag a single tab out of a tile yet. Tiles move as a whole.
- Switching layouts ends the shells running in the old layout.
- The builds aren't code signed, so Windows SmartScreen may warn you before running them.
- There's no macOS build yet.
- The Linux build has only been tested under WSL with X11, not on a full desktop. Native Wayland, the app launcher entry and the icon are untested.
- With `check = false`, leftover files from a failed update stay in the install folder until checks are turned back on.
- The File Explorer integration replaces any other program's property handler for `.md` files while installed; uninstalling AtlasWriter puts it back. Explorer windows that were already open may need reopening to show the new details.
- Linux file attributes have only been checked to build; Dolphin showing them is untested.
