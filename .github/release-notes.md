The first release of AtlasWriter, a minimalist text editor for the thinking mind.

## Features

**Markdown**
- Switch between source and rendered views with Ctrl+E
- Soft wrap and a readable centered column
- Heading outline with collapsing
- Images in the rendered view

**Code**
- Syntax highlighting chosen by file extension
- Line numbers and current line highlight
- Built-in terminal (Ctrl+J)

**Configuration**
- Everything lives in `Documents/AtlasWriter/settings.ccl`, written in the Categorical Configuration Language
- Changes apply as soon as you save the file
- Custom shortcuts, and a Mod key you can set to Ctrl, Shift or Super

**Updates**
- AtlasWriter checks for a new version when it starts, downloads it in the background and applies it when you close the app
- Set `check = false` under `updates` in `settings.ccl` to turn this off

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
