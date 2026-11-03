<p align="center"><img src="assets/logo-transparent.png" alt="The AtlasWriter logo: two hands holding up a stone marked Aw" width="220"></p>

# AtlasWriter

A minimalist text editor for the thinking mind

## A markdown editor that doesn't suck

AtlasWriter was first conceived of my personal exhaustion with the mediocrity of markdown editor projects, most of which are either lazy or unintuitive. It's clear to me that if all you want is a markdown editor that can make the best uses of what the format has to offer you either have to go with obsidian (a massive tool that is aimed at doing much more than just writing markdown) or you have to build your own, so here i go.

The markdown functionality is in it's infancy and i have plans for things i want to support in future, which you can find in the [roadmap](ROADMAP.md).
	
## A code editor that doesn't get in the way

AtlasWriter also has support for viewing/editing code, it can detect when a code file is being open through it's file extension and use appropriate Syntax highlighting, it has a terminal window for easy access, and eventually it will have support for LSPs for common languages.

## And it's all driven by a novel and elegant configuration language

As an experiment, AtlasWriter was built to try and make use of a configuration language made by Dimitrii Kovanikov in his article "The Most Elegant Configuration Language" in which he outlines how Category Theory can be used to structure data in the simplest possible way, while retaining a user friendly syntax. 

This is something i'm currently still experimenting with so you should expect there to be changes soon as i figure out how i want to tackle using this. But for now, building AtlasWriter places an example file, `settings.example.ccl`, next to the executable so you can see every option and its default. Additionally, when you first open AtlasWriter it will create `Documents/AtlasWriter/settings.ccl`, filled with those defaults, and that is the file you should use to change your preferences.

The same language describes your documents too. A `meta` block at the very top of a markdown file holds its metadata:

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

AtlasWriter shows the title in the window title and the tags, date and author in the status bar, and hides the block in the rendered view. File managers see it too: Windows File Explorer shows the title, tags, author, description and dates in the details pane, the Properties dialog and search, and on Linux they are saved as file attributes that KDE's Dolphin shows. You can also use `created` and `updated` instead of `date`, write tags as `tags = work, ideas`, and add any other fields you like.

## Install

Download the archive for your system from the [latest release](https://github.com/DefectiveNull/AtlasWriter/releases/latest), extract it, and run the install script from the extracted `AtlasWriter` folder. Linux needs no root rights. On Windows, everything installs for your user only, but showing markdown metadata in File Explorer needs admin rights once, so you'll see one Windows prompt (UAC) for that step; saying no just skips it.

- **Windows 10 or later:** `powershell -ExecutionPolicy Bypass -File install.ps1`, which adds AtlasWriter to the Start menu.
- **Linux (x64):** `sh install.sh`, which adds AtlasWriter to your app launcher.

AtlasWriter installs into a single folder in your user profile: `%LOCALAPPDATA%\Programs\AtlasWriter` on Windows and `~/.local/share/AtlasWriter` on Linux.

### Updates

AtlasWriter checks for a new version on GitHub when it starts, downloads it in the background and applies it when you close the app. On Linux this needs `curl`. To turn it off, add this to your `settings.ccl`:

```
updates =
  check = false
```

### Uninstall

Remove AtlasWriter from Settings > Apps on Windows (it asks for admin rights once, to remove the File Explorer integration), or run `sh ~/.local/share/AtlasWriter/uninstall.sh` on Linux. Your settings in `Documents/AtlasWriter` are kept.

## License

MIT License

Copyright (c) 2026 DefectiveNull

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

The bundled fonts in `assets/fonts` are under the SIL Open Font License (see the
`OFL` files next to them), and the stb and NanoSVG libraries in `third_party` keep
their own license terms.
