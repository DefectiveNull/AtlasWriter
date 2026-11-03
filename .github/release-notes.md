AtlasWriter 1.1.1 adds document metadata: a `meta` block at the top of a markdown file, written in the same CCL language as `settings.ccl`.

## What's new

**Meta blocks**

````
```meta
title = My Notes
tags =
  = work
  = ideas
date = 2026-11-02
author = Your Name
description = What this file is about
```
````

- The window title shows the document's `title` instead of its file name.
- The status bar shows its tags, date and author, for example `#work #ideas · 2026-11-02 · Your Name`.
- The rendered view (Ctrl+E) hides the block.
- `created` and `updated` work alongside `date`, tags can also be written as `tags = work, ideas`, and any other field is kept.

**File Explorer and file managers**
- Windows File Explorer shows a markdown file's title, tags, author, description and dates in the details pane, the Properties dialog and search. The installer asks for admin rights once (a UAC prompt) to turn this on; saying no only skips this part.
- On Linux, saving a markdown file stores its metadata as file attributes, which KDE's Dolphin shows and searches.

**Fixes**
- YAML front matter (a `---` block at the top, used by Jekyll, Hugo and Obsidian) now shows as a plain block in the rendered view instead of lines and run-together text, and its `#` comments no longer appear in the outline.

## Updating from 1.1.0

AtlasWriter updates itself the next time you start and close it. Updates don't run the installer, so to turn on the File Explorer integration afterwards, run this once in PowerShell:

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

- The builds aren't code signed, so Windows SmartScreen may warn you before running them.
- There's no macOS build yet.
- The Linux build has only been tested under WSL with X11, not on a full desktop. Native Wayland, the app launcher entry and the icon are untested.
- With `check = false`, leftover files from a failed update stay in the install folder until checks are turned back on.
- The File Explorer integration replaces any other program's property handler for `.md` files while installed; uninstalling AtlasWriter puts it back. Explorer windows that were already open may need reopening to show the new details.
- Linux file attributes have only been checked to build; Dolphin showing them is untested.
