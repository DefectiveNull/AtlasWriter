AtlasWriter 1.3 opens more than text. Drop files and links into your notes, then click them to open PDFs, books, web pages and audio in tiles right beside your writing.

## What's new

**Drag and drop, and links**
- Drop a file onto a document and it's written as a link, on its own line. Files from outside the document's folder are copied into an `assets` folder next to it.
- Drop a link from your browser, or paste a URL, and it becomes a Markdown link.
- In the rendered view, images show inline, and other files and websites show as cards: a file's name, type and size, or a website's address.
- Several files dropped at once go in together, and one Ctrl+Z takes them all out.
- A document that isn't saved yet keeps its dropped files in a temporary folder. They move next to it when you first save, and Save As copies them too.
- Dropping a file onto a terminal types its path into the shell.

**Opening files and links**
- Click a link or a card in the rendered view, use File > Open, or drop a file onto a tile of its kind, and it opens in the right tile. If there isn't one yet, the current tile splits to make room.
- File > Open Location... (Ctrl+L) opens a URL, a file or a web search.
- Text, Markdown and code open in the editor. Anything else opens in your system's default program.

**Reader: PDFs and EPUB books**
- PDFs show every page in one scrolling column, fitted to the tile's width. Zoom with +, - and 0, Ctrl+wheel, or the toolbar.
- EPUB books show one chapter at a time, with their pictures. The arrow keys, the toolbar, or paging past the end of a chapter turn chapters, and links between chapters stay in the book.

**Web: HTML files and websites**
- Local `.html` files are drawn by AtlasWriter itself. If the file is open in an editor, the page updates as you type.
- On Windows, websites open in a real browser view (Microsoft Edge WebView2), with back, forward, reload and an address bar. "Browser view" shows local files there too.
- Links that open new windows open as new tabs instead.

**Audio: WAV, Ogg Vorbis, MP3 and FLAC**
- A waveform you can click or drag on to seek.
- Space plays and pauses, the arrow keys skip 5 seconds, [ and ] change the speed from 0.5x to 2x, and Up and Down change the volume.
- Only the tab you're looking at plays.

**New tiles everywhere**
- Reader, Web and Audio are in the View menu and in every tile's type menu.
- Layouts in `settings.ccl` can use `reader`, `web` and `audio`.

## Updating from 1.2

AtlasWriter updates itself the next time you start and close it. The update adds `pdfium.dll` (or `libpdfium.so`) next to AtlasWriter, which PDFs need.

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

- Websites open in your web browser on Linux; the browser view is Windows only so far. Local HTML files work everywhere.
- Web pages need the Microsoft Edge WebView2 Runtime, which comes with Windows 10 and 11.
- Password-protected PDFs, and Ogg files using the Opus codec, can't be opened yet.
- An MP3's length shows once its waveform has finished reading.
- The rendered view of HTML and books handles common tags; page styles (CSS) and scripts are ignored.
- You can't drag a single tab out of a tile yet. Tiles move as a whole.
- The builds aren't code signed, so Windows SmartScreen may warn you before running them.
- There's no macOS build yet.
- The Linux build has only been tested under WSL with X11, and the new tiles haven't been tried on Linux at all.
