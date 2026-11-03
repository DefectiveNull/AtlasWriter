#include "App.h"

#include "config/Commands.h"
#include "text/GlyphAtlas.h"
#include "text/TextRenderer.h"
#include "ui/Draw.h"
#include "ui/ImageCache.h"
#include "ui/MenuBar.h"
#include "ui/Picker.h"
#include "ui/PopupMenu.h"
#include "update/Updater.h"

#include <nanosvg.h>
#include <nanosvgrast.h>

#include <algorithm>
#include <iterator>
#include <cmath>

namespace {

enum DialogKind : Sint32 {
    kDialogOpen = 1,
    kDialogSave,
};

enum PickerId : int {
    kPickLayout = 1,
    kPickSaveLayout,
};

constexpr float kHeadingScale[] = {2.0f, 1.6f, 1.3f, 1.12f}; // h1..h4, relative to the reading font
constexpr const char* kUtf8Bom = "\xEF\xBB\xBF";
constexpr float kResizeStep = 0.05f; // of the split, per Alt+Ctrl+arrow

// Must stay valid until the dialog callback runs, hence static storage
// (the extension lists are static strings too).
const SDL_DialogFileFilter* fileFilters()
{
    static const SDL_DialogFileFilter filters[] = {
        {"All supported files", supportedExtensions()},
        {"Markdown", markdownExtensions()},
        {"Source code", codeExtensions()},
        {"Plain text", "txt;text;log"},
        {"All files", "*"},
    };
    return filters;
}
constexpr int kFileFilterCount = 5;

SDL_Time modifyTime(const std::string& path)
{
    SDL_PathInfo info{};
    return SDL_GetPathInfo(path.c_str(), &info) ? info.modify_time : 0;
}

// Rasterizes icon.svg, which sits next to the executable, into the window icon.
void setWindowIcon(SDL_Window* window)
{
    constexpr int kIconSize = 64;
    const std::string path = std::string(SDL_GetBasePath()) + "icon.svg";
    size_t size = 0;
    char* text = static_cast<char*>(SDL_LoadFile(path.c_str(), &size)); // null-terminated
    if (!text) {
        SDL_Log("Could not load the window icon: %s", SDL_GetError());
        return;
    }
    NSVGimage* image = nsvgParse(text, "px", 96.0f);
    SDL_free(text);
    NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
    SDL_Surface* icon = SDL_CreateSurface(kIconSize, kIconSize, SDL_PIXELFORMAT_RGBA32);
    if (image && rasterizer && icon && image->width > 0.0f) {
        nsvgRasterize(rasterizer, image, 0.0f, 0.0f, kIconSize / image->width, static_cast<unsigned char*>(icon->pixels),
                      kIconSize, kIconSize, icon->pitch);
        if (!SDL_SetWindowIcon(window, icon))
            SDL_Log("Could not set the window icon: %s", SDL_GetError());
    }
    SDL_DestroySurface(icon);
    nsvgDeleteRasterizer(rasterizer);
    nsvgDelete(image);
}

// Documents/AtlasWriter/settings.ccl, where people will find it. Falls back to
// the preferences folder on systems with no usable Documents folder; empty if
// there is neither.
std::string findSettingsPath()
{
    constexpr const char* kFileName = "settings.ccl";
    // Linux has no Documents folder on record without user-dirs.dirs. Both are
    // owned by SDL.
    const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME);
    std::string folder;
    if (documents)
        folder = std::string(documents) + "AtlasWriter";
    else if (home)
        folder = std::string(home) + "Documents/AtlasWriter";
    if (!folder.empty()) {
        if (SDL_CreateDirectory(folder.c_str()))
            return folder + "/" + kFileName;
        SDL_Log("Could not create %s: %s", folder.c_str(), SDL_GetError());
    }
    std::string path;
    // Not "AtlasWriter/AtlasWriter": on Linux that is the installed executable.
    if (char* prefs = SDL_GetPrefPath("", "AtlasWriterSettings")) {
        path = std::string(prefs) + kFileName;
        SDL_free(prefs);
    }
    return path;
}

bool readFile(const std::string& path, std::string& out)
{
    size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (!data)
        return false;
    out.assign(static_cast<const char*>(data), size);
    SDL_free(data);
    return true;
}

// The current working directory, for shells started with no file open.
std::string workingDirectory()
{
    std::string directory;
    if (char* cwd = SDL_GetCurrentDirectory()) {
        directory = cwd;
        SDL_free(cwd);
    }
    return directory;
}

} // namespace

App::App() = default;

App::~App()
{
    // GPU resources must go before the renderer that owns them.
    updater_.reset();                        // cancels any download
    focused_ = lastEditor_ = nullptr;
    std::unique_ptr<TileTree::Node> root = tiles_.takeRoot(); // stops shells, frees views
    root.reset();
    images_.reset(); // joins the decode thread, frees textures
    typeMenu_.reset();
    picker_.reset();
    menu_.reset();
    for (auto& text : text_)
        text.reset();
    batch_.reset();
    atlas_.reset();
    SDL_DestroyCursor(arrowCursor_);
    SDL_DestroyCursor(textCursor_);
    SDL_DestroyCursor(pointerCursor_);
    SDL_DestroyCursor(nsResizeCursor_);
    SDL_DestroyCursor(ewResizeCursor_);
    SDL_DestroyCursor(moveCursor_);
    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
    SDL_Quit();
}

bool App::init(int argc, char* argv[])
{
    // SDL's defaults plus hide_numpad, so numpad keys with NumLock off arrive
    // as Home/End/arrows instead of SDLK_KP_* and navigate like the main keys.
    SDL_SetHint(SDL_HINT_KEYCODE_OPTIONS, "french_numbers,latin_letters,hide_numpad");

    // The id ties the window to its launcher entry on Linux desktops.
    SDL_SetAppMetadata("AtlasWriter", ATLASWRITER_VERSION, "atlaswriter");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    if (!SDL_CreateWindowAndRenderer("AtlasWriter", 1024, 700,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                     &window_, &renderer_)) {
        SDL_Log("SDL_CreateWindowAndRenderer failed: %s", SDL_GetError());
        return false;
    }
    setWindowIcon(window_);
    SDL_SetRenderVSync(renderer_, 1);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_Log("Renderer: %s", SDL_GetRendererName(renderer_));

    dialogEvent_ = SDL_RegisterEvents(4);
    imageEvent_ = dialogEvent_ + 1;
    terminalEvent_ = dialogEvent_ + 2;
    updateEvent_ = dialogEvent_ + 3;
    if (dialogEvent_ == 0) {
        SDL_Log("SDL_RegisterEvents failed: %s", SDL_GetError());
        return false;
    }

    if (!loadFonts())
        return false;

    atlas_ = std::make_unique<GlyphAtlas>(renderer_);
    batch_ = std::make_unique<TextBatch>(renderer_, *atlas_);
    for (int i = 0; i < kFaceCount; ++i)
        text_[i] = std::make_unique<TextRenderer>(*batch_, *atlas_, fonts_[i]);

    images_ = std::make_unique<ImageCache>(renderer_, imageEvent_);
    ctx_.code = text_[kFaceCode].get();
    ctx_.reading = text_[kFaceRegular].get();
    ctx_.markdown.regular = text_[kFaceRegular].get();
    ctx_.markdown.bold = text_[kFaceBold].get();
    ctx_.markdown.italic = text_[kFaceItalic].get();
    ctx_.markdown.boldItalic = text_[kFaceBoldItalic].get();
    ctx_.markdown.code = text_[kFaceCode].get();
    ctx_.markdown.heading[0] = text_[kFaceH1].get();
    ctx_.markdown.heading[1] = text_[kFaceH2].get();
    ctx_.markdown.heading[2] = text_[kFaceH3].get();
    ctx_.markdown.heading[3] = text_[kFaceH4].get();
    ctx_.markdown.heading[4] = text_[kFaceBold].get();
    ctx_.markdown.heading[5] = text_[kFaceBold].get();
    ctx_.images = images_.get();
    ctx_.terminalEvent = terminalEvent_;
    ctx_.displayScale = SDL_GetWindowDisplayScale(window_);

    menu_ = std::make_unique<MenuBar>(fonts_[kFaceCode], *text_[kFaceCode]);
    picker_ = std::make_unique<Picker>(*text_[kFaceCode]);
    typeMenu_ = std::make_unique<PopupMenu>(*text_[kFaceCode]);

    arrowCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    textCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    pointerCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    nsResizeCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
    ewResizeCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
    moveCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE);
    currentCursor_ = arrowCursor_;

    // Built-in defaults first, so a broken settings file still leaves
    // working shortcuts; then the user's file on top.
    {
        std::vector<std::string> ignored;
        ccl::Error error;
        Settings::load(Settings::defaultText(), settings_, ignored, error);
    }
    settingsPath_ = findSettingsPath();
    applySettings();
    loadSettings();
    SDL_StartTextInput(window_);

    // AtlasWriter [--preview] [file]
    const char* file = nullptr;
    bool preview = false;
    std::string updateFeed;
    for (int i = 1; i < argc; ++i) {
        if (SDL_strcmp(argv[i], "--preview") == 0)
            preview = true;
#ifndef NDEBUG
        else if (SDL_strcmp(argv[i], "--update-feed") == 0 && i + 1 < argc)
            updateFeed = argv[++i]; // tests the updater against a local folder
#endif
        else
            file = argv[i];
    }

    auto document = std::make_shared<Document>();
    if (file) {
        std::string error;
        if (!document->load(file, error))
            showError(std::string("Could not open \"") + file + "\":\n" + error);
    }
    const Layout* startup = findLayout(settings_.layouts, settings_.layout);
    if (!startup)
        startup = findLayout(settings_.layouts, "default");
    applyLayout(*startup, {document}, document);
    if (preview) {
        if (EditorWorkspace* editor = targetEditor())
            editor->setPreview(true);
    }

    if (settings_.updateCheck) {
        updater_ = std::make_unique<Updater>();
        updater_->start(updateEvent_, std::move(updateFeed));
    }
    return true;
}

void App::run()
{
    // Event-driven loop: block until something happens instead of spinning,
    // waking only for caret blinks, so an idle editor uses ~0% CPU.
    while (running_) {
        if (dirty_) {
            draw();
            dirty_ = false;
        }

        SDL_Event event;
        const EditorWorkspace* editor = focusedEditor();
        const Sint32 timeout = editor && windowFocused_ ? editor->blinkTimeoutMs() : -1;
        const bool gotEvent = timeout < 0 ? SDL_WaitEvent(&event) : SDL_WaitEventTimeout(&event, timeout);
        if (!gotEvent) {
            dirty_ = true; // caret blink
            continue;
        }

        do {
            handleEvent(event);
        } while (running_ && SDL_PollEvent(&event));
    }
}

// ---------------------------------------------------------------------------
// Events and commands
// ---------------------------------------------------------------------------

void App::handleEvent(SDL_Event& event)
{
    if (event.type == dialogEvent_) {
        onDialogResult(event.user);
        return;
    }
    if (event.type == updateEvent_) {
        dirty_ = true; // shows the note in the status bar
        return;
    }
    if (event.type == imageEvent_) {
        // Decoded images change sizes in rendered views.
        if (images_->processLoaded()) {
            tiles_.forEachTile([](Tile& tile) { tile.workspace->imagesLoaded(); });
            dirty_ = true;
        }
        return;
    }
    if (event.type == terminalEvent_) {
        // Hidden terminals read their output too, so their screens keep up.
        tiles_.forEachTile([&](Tile& tile) {
            if (tile.workspace->type() == WorkspaceType::Terminal)
                dirty_ |= static_cast<TerminalWorkspace&>(*tile.workspace).onOutput();
            for (const auto& hidden : tile.stash) {
                if (hidden->type() == WorkspaceType::Terminal)
                    static_cast<TerminalWorkspace&>(*hidden).onOutput();
            }
        });
        return;
    }

    SDL_ConvertEventToRenderCoordinates(renderer_, &event);

    switch (event.type) {
    case SDL_EVENT_QUIT:
        request({Action::Kind::Exit});
        return;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        checkSettingsFile(); // maybe edited in another program
        windowFocused_ = true;
        updateFocus();
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        windowFocused_ = false;
        updateFocus();
        break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        applyFontSize();
        return;
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        dirty_ = true;
        return;
    case SDL_EVENT_MOUSE_MOTION:
        if (draggingDivider_) {
            tiles_.dragDivider(divider_, event.motion.x, event.motion.y);
            dirty_ = true;
            return;
        }
        if (tileDrag_ != TileDrag::None) {
            const float dx = event.motion.x - dragStartX_, dy = event.motion.y - dragStartY_;
            if (tileDrag_ == TileDrag::Pending && dx * dx + dy * dy > 16.0f) {
                tileDrag_ = TileDrag::Moving;
                SDL_CaptureMouse(true);
            }
            if (tileDrag_ == TileDrag::Moving) {
                drop_ = tiles_.dropAt(event.motion.x, event.motion.y, dragTile_);
                dirty_ = true;
            }
            updateMouseCursor(event.motion.x, event.motion.y);
            return;
        }
        updateMouseCursor(event.motion.x, event.motion.y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (draggingDivider_ && event.button.button == SDL_BUTTON_LEFT) {
            draggingDivider_ = false;
            SDL_CaptureMouse(false);
            return;
        }
        if (tileDrag_ != TileDrag::None && event.button.button == SDL_BUTTON_LEFT) {
            if (tileDrag_ == TileDrag::Moving)
                drop_ = tiles_.dropAt(event.button.x, event.button.y, dragTile_);
            endTileDrag(tileDrag_ == TileDrag::Moving);
            updateMouseCursor(event.button.x, event.button.y);
            return;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (tileDrag_ != TileDrag::None) { // another button cancels the drag
            endTileDrag(false);
            updateMouseCursor(event.button.x, event.button.y);
            return;
        }
        break;
    case SDL_EVENT_KEY_DOWN:
        if (tileDrag_ != TileDrag::None) {
            if (event.key.key == SDLK_ESCAPE)
                endTileDrag(false);
            return; // no typing into a tile while it's being dragged
        }
        break;
    default:
        break;
    }

    // A tile's type menu, then the layout picker, take all input while open.
    if (typeMenu_->isOpen()) {
        const bool input = event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_TEXT_INPUT
                        || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP
                        || event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_WHEEL
                        || event.type == SDL_EVENT_WINDOW_FOCUS_LOST;
        if (input) {
            int chosen = 0;
            dirty_ |= typeMenu_->handleEvent(event, chosen);
            Tile* tile = typeMenuTile_;
            if (!typeMenu_->isOpen())
                typeMenuTile_ = nullptr;
            if (chosen > 0 && tile && tiles_.contains(tile))
                setTileType(tile, kWorkspaceTypes[chosen - 1]);
            if (event.type == SDL_EVENT_MOUSE_MOTION)
                updateMouseCursor(event.motion.x, event.motion.y);
            return;
        }
    }
    if (picker_->isOpen()) {
        const bool input = event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_TEXT_INPUT
                        || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_MOTION
                        || event.type == SDL_EVENT_MOUSE_WHEEL;
        if (input) {
            dirty_ |= picker_->handleEvent(event);
            std::string choice;
            if (picker_->takeResult(choice))
                onPicked(picker_->id(), choice);
            return;
        }
    }

    int command = 0;
    bool redraw = false;
    const bool consumed = menu_->handleEvent(event, command, redraw);
    dirty_ |= redraw;
    if (command != 0)
        runCommand(command);
    if (consumed)
        return;

    // A key that ran a shortcut doesn't type anything too (Shift+S as Mod+S
    // would still produce an "S"). Its text, if any, comes right after it.
    if (event.type == SDL_EVENT_KEY_DOWN) {
        skipTextInput_ = handleShortcut(event.key);
        if (skipTextInput_)
            return;
    }
    if (event.type == SDL_EVENT_TEXT_INPUT && skipTextInput_) {
        skipTextInput_ = false;
        return;
    }

    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_TEXT_INPUT) {
        if (focused_) {
            dirty_ |= focused_->workspace->handleEvent(event);
            afterTileEvent(*focused_);
        }
        return;
    }
    handleMouse(event);
}

// Clicks go to the tile under the pointer (and focus it); motion, wheel and
// button releases go to every tile, each of which checks its own bounds and
// drags, so a selection dragged out of a tile still ends.
void App::handleMouse(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        const float x = event.button.x, y = event.button.y;
        if (event.button.button == SDL_BUTTON_LEFT && tiles_.dividerAt(x, y, divider_)) {
            draggingDivider_ = true;
            SDL_CaptureMouse(true);
            return;
        }
        Tile* tile = tiles_.tileAt(x, y);
        if (!tile)
            return;
        const TileTree::HeaderHit hit = tiles_.headerAt(*tile, x, y, *text_[kFaceCode]);
        const bool left = event.button.button == SDL_BUTTON_LEFT;
        switch (hit.part) {
        case TileTree::HeaderHit::Part::None:
            break;
        case TileTree::HeaderHit::Part::Type:
            if (left) {
                focusTile(tile);
                openTypeMenu(tile, false);
            }
            dirty_ = true;
            return;
        case TileTree::HeaderHit::Part::Close:
            if (left && tiles_.tileCount() > 1) {
                Action close{Action::Kind::CloseTile};
                close.tile = tile;
                request(close);
            }
            dirty_ = true;
            return;
        case TileTree::HeaderHit::Part::Tab:
            if (left) {
                tile->workspace->selectTab(hit.tab);
                focusTile(tile);
            } else if (event.button.button == SDL_BUTTON_MIDDLE) {
                Action close{Action::Kind::CloseTab};
                close.tile = tile;
                close.tab = hit.tab;
                request(close);
            }
            dirty_ = true;
            return;
        case TileTree::HeaderHit::Part::Handle:
            if (left) {
                focusTile(tile);
                if (tiles_.tileCount() > 1) { // a lone tile has nowhere to go
                    tileDrag_ = TileDrag::Pending;
                    dragTile_ = tile;
                    dragStartX_ = x;
                    dragStartY_ = y;
                    drop_ = {};
                }
            }
            dirty_ = true;
            return;
        }
        if (tile != focused_)
            focusTile(tile);
        dirty_ |= tile->workspace->handleEvent(event);
        afterTileEvent(*tile);
        return;
    }
    const bool broadcast = event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_WHEEL
                        || event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE;
    if (!broadcast)
        return;
    for (Tile* tile : tiles_.tiles())
        dirty_ |= tile->workspace->handleEvent(event);
}

void App::afterTileEvent(Tile& tile)
{
    switch (tile.workspace->type()) {
    case WorkspaceType::Outline: {
        size_t line = 0;
        if (static_cast<OutlineWorkspace&>(*tile.workspace).takeJump(line)) {
            if (EditorWorkspace* editor = targetEditor())
                editor->goToSourceLine(line);
            dirty_ = true;
        }
        break;
    }
    default:
        break;
    }
}

// Shortcuts come from the settings' keymap. While a terminal has focus, the
// keys of commands a shell uses too (Ctrl+C, Ctrl+W, Ctrl+E, ...) belong
// to the shell.
bool App::handleShortcut(const SDL_KeyboardEvent& key)
{
    const Command command = settings_.commandFor(key);
    if (command == kCmdNone)
        return false;
    const bool shellKey = command == kCmdUndo || command == kCmdRedo || command == kCmdCut || command == kCmdCopy
                       || command == kCmdPaste || command == kCmdSelectAll || command == kCmdCloseTab
                       || command == kCmdTogglePreview;
    if (shellKey && focusedTerminal())
        return false;
    runCommand(command);
    return true;
}

void App::runCommand(int command)
{
    EditorWorkspace* editor = targetEditor();
    const auto directionOf = [](int base, int c) { return static_cast<Direction>(c - base); };
    switch (command) {
    case kCmdNew:
        if (TerminalWorkspace* terminal = focusedTerminal())
            terminal->addShell(currentDocument() && !currentDocument()->directory().empty()
                                   ? currentDocument()->directory()
                                   : workingDirectory());
        else
            newDocument();
        break;
    case kCmdOpen:
        showOpenDialog();
        break;
    case kCmdSave:
        if (std::shared_ptr<Document> doc = currentDocument())
            save(doc);
        break;
    case kCmdSaveAs:
        if (std::shared_ptr<Document> doc = currentDocument())
            showSaveDialog(doc);
        break;
    case kCmdPreferences:
        openSettings();
        break;
    case kCmdExit:
        request({Action::Kind::Exit});
        break;
    case kCmdZoomIn:
        fontPoints_ = std::min(fontPoints_ + 1.0f, 72.0f);
        applyFontSize();
        break;
    case kCmdZoomOut:
        fontPoints_ = std::max(fontPoints_ - 1.0f, 6.0f);
        applyFontSize();
        break;
    case kCmdZoomReset:
        fontPoints_ = settings_.fontSize;
        applyFontSize();
        break;
    case kCmdGlyphAtlas:
        showAtlas_ = !showAtlas_;
        break;
    case kCmdTogglePreview:
        if (editor)
            editor->setPreview(!editor->previewing());
        break;
    case kCmdUndo:
        if (editor && editor->editor().undo() && editor->previewing())
            editor->refreshPreview();
        break;
    case kCmdRedo:
        if (editor && editor->editor().redo() && editor->previewing())
            editor->refreshPreview();
        break;
    case kCmdCut:
        if (editor && !editor->previewing())
            editor->editor().cut();
        break;
    case kCmdCopy:
        if (editor && !editor->previewing())
            editor->editor().copy();
        break;
    case kCmdPaste:
        if (editor && !editor->previewing())
            editor->editor().paste();
        break;
    case kCmdSelectAll:
        if (editor && !editor->previewing())
            editor->editor().selectAll();
        break;
    case kCmdCollapseAll:
    case kCmdExpandAll: {
        MarkdownView* view = nullptr;
        if (focused_ && focused_->workspace->type() == WorkspaceType::Preview)
            view = &static_cast<PreviewWorkspace&>(*focused_->workspace).view();
        else if (editor && editor->previewing())
            view = &editor->markdown();
        if (view && command == kCmdCollapseAll)
            view->collapseAll();
        else if (view)
            view->expandAll();
        break;
    }
    case kCmdShowEditor:
        toggleTile(WorkspaceType::Editor);
        break;
    case kCmdShowPreview:
        toggleTile(WorkspaceType::Preview);
        break;
    case kCmdToggleOutline:
        toggleTile(WorkspaceType::Outline);
        break;
    case kCmdToggleTerminal:
        toggleTile(WorkspaceType::Terminal);
        break;
    case kCmdFocusLeft:
    case kCmdFocusRight:
    case kCmdFocusUp:
    case kCmdFocusDown:
        if (focused_) {
            layout();
            if (Tile* next = tiles_.neighbor(focused_, directionOf(kCmdFocusLeft, command)))
                focusTile(next);
        }
        break;
    case kCmdMoveLeft:
    case kCmdMoveRight:
    case kCmdMoveUp:
    case kCmdMoveDown:
        if (focused_)
            tiles_.move(focused_, directionOf(kCmdMoveLeft, command));
        break;
    case kCmdResizeLeft:
    case kCmdResizeRight:
    case kCmdResizeUp:
    case kCmdResizeDown:
        if (focused_)
            tiles_.resize(focused_, directionOf(kCmdResizeLeft, command), kResizeStep);
        break;
    case kCmdSplitHorizontal:
        splitFocused(true);
        break;
    case kCmdSplitVertical:
        splitFocused(false);
        break;
    case kCmdCloseTile:
        if (focused_ && tiles_.tileCount() > 1) {
            Action close{Action::Kind::CloseTile};
            close.tile = focused_;
            request(close);
        }
        break;
    case kCmdNextTab:
    case kCmdPrevTab:
        if (focused_ && focused_->workspace->tabCount() > 1) {
            Workspace& ws = *focused_->workspace;
            const size_t n = ws.tabCount();
            ws.selectTab((ws.activeTab() + (command == kCmdNextTab ? 1 : n - 1)) % n);
        }
        break;
    case kCmdCloseTab:
        if (focused_) {
            Action close{Action::Kind::CloseTab};
            close.tile = focused_;
            close.tab = focused_->workspace->activeTab();
            request(close);
        }
        break;
    case kCmdChangeTileType:
        if (focused_)
            openTypeMenu(focused_, true);
        break;
    case kCmdSwitchLayout: {
        std::vector<std::string> names;
        for (const Layout& l : settings_.layouts)
            names.push_back(l.name);
        picker_->open(kPickLayout, "Layout:", std::move(names), false);
        break;
    }
    case kCmdSaveLayout: {
        if (settingsPath_.empty()) {
            showError("There is no settings file to save layouts in on this system.");
            break;
        }
        std::vector<std::string> names;
        for (const Layout& l : settings_.layouts)
            names.push_back(l.name);
        picker_->open(kPickSaveLayout, "Save layout as:", std::move(names), true);
        break;
    }
    default:
        break;
    }
    dirty_ = true;
}

void App::onPicked(int id, const std::string& choice)
{
    if (id == kPickLayout) {
        Action action{Action::Kind::SwitchLayout};
        action.layout = choice;
        request(action);
    } else if (id == kPickSaveLayout) {
        if (choice.find_first_of("=\n\r") != std::string::npos || choice == "/") {
            showError("A layout name can't contain \"=\" or line breaks.");
            return;
        }
        saveLayoutAs(choice);
    }
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// Unsaved changes
// ---------------------------------------------------------------------------

std::vector<std::shared_ptr<Document>> App::droppedDocuments(const Action& action) const
{
    // The editor tabs that go away (a closed tile takes its hidden editors
    // with it)...
    std::vector<std::pair<const Tile*, size_t>> dropped; // (tile, tab); tab SIZE_MAX = every tab
    switch (action.kind) {
    case Action::Kind::None:
        return {};
    case Action::Kind::Exit:
        dropped.push_back({nullptr, SIZE_MAX}); // everything
        break;
    case Action::Kind::CloseTab:
        dropped.push_back({action.tile, action.tab});
        break;
    case Action::Kind::CloseTile:
        dropped.push_back({action.tile, SIZE_MAX});
        break;
    case Action::Kind::SwitchLayout: {
        // Documents move to the new layout's editors; only a layout without
        // any drops them.
        const Layout* layout = findLayout(settings_.layouts, action.layout);
        const std::function<bool(const LayoutNode&)> hasEditor = [&](const LayoutNode& n) {
            if (n.tile)
                return n.type == WorkspaceType::Editor;
            return std::any_of(n.children.begin(), n.children.end(), hasEditor);
        };
        if (!layout || hasEditor(layout->root))
            return {};
        dropped.push_back({nullptr, SIZE_MAX});
        break;
    }
    }
    const auto isDropped = [&](const Tile* tile, size_t tab, bool visible) {
        for (const auto& [t, i] : dropped) {
            // A single tab is always one of the visible editor's.
            if ((t == nullptr || t == tile) && (i == SIZE_MAX || (visible && i == tab)))
                return true;
        }
        return false;
    };

    // ...and the documents only they show.
    std::vector<std::shared_ptr<Document>> candidates, kept;
    forEachEditor([&](Tile& tile, EditorWorkspace& editor, bool visible) {
        for (size_t i = 0; i < editor.tabCount(); ++i) {
            const std::shared_ptr<Document>& doc = editor.documentAt(i);
            auto& list = isDropped(&tile, i, visible) ? candidates : kept;
            if (std::find(list.begin(), list.end(), doc) == list.end())
                list.push_back(doc);
        }
    });
    std::vector<std::shared_ptr<Document>> out;
    for (const auto& doc : candidates) {
        if (doc->modified() && std::find(kept.begin(), kept.end(), doc) == kept.end())
            out.push_back(doc);
    }
    return out;
}

void App::request(const Action& action)
{
    if (dialogOpen_)
        return;
    const std::vector<std::shared_ptr<Document>> unsaved = droppedDocuments(action);
    if (unsaved.empty()) {
        perform(action);
        return;
    }

    std::string message;
    if (unsaved.size() == 1) {
        message = "Do you want to save the changes you made to " + unsaved[0]->displayName() + "?";
    } else {
        message = "Do you want to save the changes you made to these documents?\n";
        for (const auto& doc : unsaved)
            message += "\n\xE2\x80\xA2 " + doc->displayName(); // bullet
    }
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Save"},
        {0, 2, "Don't Save"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
    };
    const SDL_MessageBoxData data{
        SDL_MESSAGEBOX_WARNING | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT,
        window_, "AtlasWriter", message.c_str(),
        static_cast<int>(SDL_arraysize(buttons)), buttons, nullptr,
    };

    int choice = 0;
    if (!SDL_ShowMessageBox(&data, &choice))
        choice = 0;

    if (choice == 1) {
        pending_ = action;
        saveQueue_.assign(unsaved.begin(), unsaved.end());
        continueSaving();
    } else if (choice == 2) {
        perform(action);
    }
}

void App::continueSaving()
{
    while (!saveQueue_.empty()) {
        std::shared_ptr<Document> doc = std::move(saveQueue_.front());
        saveQueue_.pop_front();
        const SaveResult result = save(doc);
        if (result == SaveResult::Pending)
            return; // resumes when the Save As dialog closes
        if (result == SaveResult::Failed) {
            saveQueue_.clear();
            pending_ = {};
            return;
        }
    }
    const Action next = std::move(pending_);
    pending_ = {};
    perform(next);
}

void App::perform(const Action& action)
{
    switch (action.kind) {
    case Action::Kind::None:
        break;
    case Action::Kind::Exit:
        running_ = false;
        break;
    case Action::Kind::CloseTab:
        if (tiles_.contains(action.tile) && action.tab < action.tile->workspace->tabCount())
            closeTabNow(action.tile, action.tab);
        break;
    case Action::Kind::CloseTile:
        if (tiles_.contains(action.tile) && tiles_.tileCount() > 1)
            removeTile(action.tile);
        break;
    case Action::Kind::SwitchLayout:
        switchLayout(action.layout);
        break;
    }
    dirty_ = true;
}

App::SaveResult App::save(const std::shared_ptr<Document>& document)
{
    if (document->path().empty()) {
        showSaveDialog(document);
        return dialogOpen_ ? SaveResult::Pending : SaveResult::Failed;
    }
    return writeDocument(*document, document->path()) ? SaveResult::Saved : SaveResult::Failed;
}

bool App::writeDocument(Document& document, const std::string& path)
{
    std::string error;
    if (!document.save(path, error)) {
        showError("Could not save \"" + path + "\":\n" + error);
        return false;
    }
    if (samePath(path, settingsPath_))
        loadSettings(); // saving settings.ccl applies it
    dirty_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// File dialogs (results arrive as events, since callbacks may run on another thread)
// ---------------------------------------------------------------------------

namespace {

void pushDialogResult(Uint32 eventType, Sint32 kind, const char* const* files)
{
    SDL_Event event{};
    event.type = eventType;
    event.user.code = kind;
    if (!files)
        SDL_Log("File dialog failed: %s", SDL_GetError());
    else if (files[0])
        event.user.data1 = SDL_strdup(files[0]); // freed in onDialogResult
    SDL_PushEvent(&event);
}

} // namespace

void App::showOpenDialog()
{
    if (dialogOpen_)
        return;
    dialogOpen_ = true;
    SDL_ShowOpenFileDialog(&App::onOpenDialog, this, window_, fileFilters(), kFileFilterCount, nullptr, false);
}

void App::showSaveDialog(const std::shared_ptr<Document>& document)
{
    if (dialogOpen_)
        return;
    dialogOpen_ = true;
    dialogDocument_ = document;
    SDL_ShowSaveFileDialog(&App::onSaveDialog, this, window_, fileFilters(), kFileFilterCount,
                           document->path().empty() ? nullptr : document->path().c_str());
}

void SDLCALL App::onOpenDialog(void* userdata, const char* const* files, int)
{
    pushDialogResult(static_cast<App*>(userdata)->dialogEvent_, kDialogOpen, files);
}

void SDLCALL App::onSaveDialog(void* userdata, const char* const* files, int)
{
    pushDialogResult(static_cast<App*>(userdata)->dialogEvent_, kDialogSave, files);
}

void App::onDialogResult(const SDL_UserEvent& event)
{
    dialogOpen_ = false;
    char* path = static_cast<char*>(event.data1); // null when cancelled
    if (event.code == kDialogOpen) {
        if (path)
            openPath(path);
    } else if (event.code == kDialogSave) {
        std::shared_ptr<Document> doc = std::move(dialogDocument_);
        dialogDocument_.reset();
        if (path && doc && writeDocument(*doc, path)) {
            continueSaving(); // the rest of an Exit/close, if one is waiting
        } else {
            saveQueue_.clear(); // cancelled: so is whatever was waiting
            pending_ = {};
        }
    }
    SDL_free(path);
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// Documents and routing
// ---------------------------------------------------------------------------

void App::newDocument()
{
    Tile* tile = editorTile(true);
    static_cast<EditorWorkspace&>(*tile->workspace).open(std::make_shared<Document>());
    focusTile(tile);
    dirty_ = true;
}

void App::openPath(const std::string& path)
{
    // Already open: go to its tab.
    for (Tile* tile : tiles_.tiles()) {
        if (tile->workspace->type() != WorkspaceType::Editor)
            continue;
        auto& editor = static_cast<EditorWorkspace&>(*tile->workspace);
        for (size_t i = 0; i < editor.tabCount(); ++i) {
            if (!editor.documentAt(i)->path().empty() && samePath(editor.documentAt(i)->path(), path)) {
                editor.selectTab(i);
                focusTile(tile);
                dirty_ = true;
                return;
            }
        }
    }
    // Held by a hidden editor only: share that document, so the file never
    // has two diverging buffers.
    std::shared_ptr<Document> doc;
    forEachEditor([&](Tile&, EditorWorkspace& editor, bool) {
        for (size_t i = 0; i < editor.tabCount() && !doc; ++i) {
            if (!editor.documentAt(i)->path().empty() && samePath(editor.documentAt(i)->path(), path))
                doc = editor.documentAt(i);
        }
    });
    if (!doc) {
        doc = std::make_shared<Document>();
        std::string error;
        if (!doc->load(path, error)) {
            showError("Could not open \"" + path + "\":\n" + error);
            return;
        }
    }
    Tile* tile = editorTile(true);
    static_cast<EditorWorkspace&>(*tile->workspace).open(doc);
    focusTile(tile);
    dirty_ = true;
}

std::shared_ptr<Document> App::currentDocument() const
{
    const EditorWorkspace* editor = targetEditor();
    return editor ? editor->sharedDocument() : nullptr;
}

void App::showError(const std::string& message)
{
    SDL_Log("%s", message.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "AtlasWriter", message.c_str(), window_);
}

// ---------------------------------------------------------------------------
// Tiles
// ---------------------------------------------------------------------------

std::unique_ptr<Workspace> App::makeWorkspace(WorkspaceType type)
{
    switch (type) {
    case WorkspaceType::Editor:
        return std::make_unique<EditorWorkspace>(ctx_);
    case WorkspaceType::Preview:
        return std::make_unique<PreviewWorkspace>(ctx_);
    case WorkspaceType::Outline:
        return std::make_unique<OutlineWorkspace>(ctx_);
    case WorkspaceType::Terminal: {
        const std::shared_ptr<Document> doc = currentDocument();
        return std::make_unique<TerminalWorkspace>(
            ctx_, doc && !doc->directory().empty() ? doc->directory() : workingDirectory());
    }
    }
    return std::make_unique<EditorWorkspace>(ctx_);
}

// Sizes in a layout are percentages; tiles without one share what the
// others leave.
std::unique_ptr<App::Tile> App::buildTile(const LayoutNode& node, std::vector<Tile*>& editors)
{
    if (node.tile) {
        std::unique_ptr<Tile> tile = TileTree::makeTile(makeWorkspace(node.type));
        if (node.type == WorkspaceType::Editor)
            editors.push_back(tile.get());
        return tile;
    }
    auto split = std::make_unique<Tile>();
    split->horizontal = node.horizontal;
    float sized = 0.0f;
    size_t unsized = 0;
    for (const LayoutNode& child : node.children) {
        if (child.size > 0.0f)
            sized += child.size;
        else
            ++unsized;
    }
    const float left = sized < 100.0f ? 100.0f - sized : 0.0f;
    // Sized tiles that fill the split leave the others a fair share anyway.
    const float share = unsized == 0     ? 0.0f
                        : left > 0.0f ? left / static_cast<float>(unsized)
                                      : 100.0f / static_cast<float>(node.children.size());
    for (const LayoutNode& child : node.children) {
        std::unique_ptr<Tile> built = buildTile(child, editors);
        built->weight = child.size > 0.0f ? child.size : share;
        built->parent = split.get();
        split->children.push_back(std::move(built));
    }
    return split;
}

void App::applyLayout(const Layout& layout, const std::vector<std::shared_ptr<Document>>& documents,
                      const std::shared_ptr<Document>& active)
{
    std::vector<Tile*> editors;
    std::unique_ptr<Tile> root = buildTile(layout.root, editors);
    root->weight = 1.0f;

    // The first editor gets every document; the others show the active one.
    for (size_t i = 0; i < editors.size(); ++i) {
        auto& editor = static_cast<EditorWorkspace&>(*editors[i]->workspace);
        if (i == 0) {
            for (const auto& doc : documents)
                editor.open(doc);
        }
        if (active)
            editor.open(active);
        else if (editor.tabCount() == 0)
            editor.open(std::make_shared<Document>());
    }

    // Shells in the old layout end with it.
    endTileDrag(false);
    typeMenu_->close();
    typeMenuTile_ = nullptr;
    focused_ = lastEditor_ = nullptr;
    tiles_.setRoot(std::move(root));
    lastEditor_ = editors.empty() ? nullptr : editors.front();
    focusTile(editors.empty() ? tiles_.tiles().front() : editors.front());
    tiles_.forEachTile([](Tile& tile) { tile.workspace->styleChanged(); });
    dirty_ = true;
}

void App::switchLayout(const std::string& name)
{
    const Layout* layout = findLayout(settings_.layouts, name);
    if (!layout)
        return;
    // Documents carry over, the current one in front.
    std::vector<std::shared_ptr<Document>> documents;
    forEachEditor([&](Tile&, EditorWorkspace& editor, bool) {
        for (size_t i = 0; i < editor.tabCount(); ++i) {
            if (std::find(documents.begin(), documents.end(), editor.documentAt(i)) == documents.end())
                documents.push_back(editor.documentAt(i));
        }
    });
    const std::shared_ptr<Document> active = currentDocument();
    const Layout copy = *layout; // settings_ isn't touched, but stay safe across the rebuild
    applyLayout(copy, documents, active);
}

void App::saveLayoutAs(const std::string& name)
{
    // An open, edited settings.ccl would go out of sync with the file.
    std::shared_ptr<Document> open;
    forEachEditor([&](Tile&, EditorWorkspace& editor, bool) {
        for (size_t i = 0; i < editor.tabCount(); ++i) {
            if (samePath(editor.documentAt(i)->path(), settingsPath_))
                open = editor.documentAt(i);
        }
    });
    if (open && open->modified()) {
        showError("settings.ccl has unsaved changes. Save or undo them, then save the layout again.");
        return;
    }

    std::string text;
    if (!readFile(settingsPath_, text))
        text = Settings::defaultText();
    const bool bom = text.starts_with(kUtf8Bom);
    if (bom)
        text.erase(0, 3);
    std::string updated = replaceLayoutBlock(text, name, tiles_.toLayout());
    if (bom)
        updated.insert(0, kUtf8Bom);
    if (!SDL_SaveFile(settingsPath_.c_str(), updated.data(), updated.size())) {
        showError("Could not save the layout to \"" + settingsPath_ + "\":\n" + SDL_GetError());
        return;
    }
    if (open) {
        std::string error;
        open->load(settingsPath_, error); // the tab shows the new file
    }
    loadSettings();
}

void App::focusTile(Tile* tile)
{
    focused_ = tile;
    if (tile && tile->workspace->type() == WorkspaceType::Editor)
        lastEditor_ = tile;
    updateFocus();
}

void App::updateFocus()
{
    tiles_.forEachTile([&](Tile& tile) { tile.workspace->setFocused(windowFocused_ && &tile == focused_); });
    dirty_ = true;
}

App::Tile* App::editorTile(bool create)
{
    if (lastEditor_ && tiles_.contains(lastEditor_))
        return lastEditor_;
    lastEditor_ = nullptr;
    for (Tile* tile : tiles_.tiles()) {
        if (tile->workspace->type() == WorkspaceType::Editor) {
            lastEditor_ = tile;
            return tile;
        }
    }
    if (!create)
        return nullptr;
    // No editor showing: split the focused tile along its longer side.
    Tile* anchor = focused_ ? focused_ : tiles_.tiles().front();
    layout();
    const bool horizontal = anchor->rect.w >= anchor->rect.h;
    lastEditor_ = tiles_.split(anchor, horizontal, makeWorkspace(WorkspaceType::Editor));
    return lastEditor_;
}

EditorWorkspace* App::focusedEditor() const
{
    if (focused_ && focused_->workspace->type() == WorkspaceType::Editor)
        return &static_cast<EditorWorkspace&>(*focused_->workspace);
    return nullptr;
}

EditorWorkspace* App::targetEditor() const
{
    if (EditorWorkspace* editor = focusedEditor())
        return editor;
    if (lastEditor_ && tiles_.contains(lastEditor_))
        return &static_cast<EditorWorkspace&>(*lastEditor_->workspace);
    for (Tile* tile : tiles_.tiles()) {
        if (tile->workspace->type() == WorkspaceType::Editor)
            return &static_cast<EditorWorkspace&>(*tile->workspace);
    }
    return nullptr;
}

TerminalWorkspace* App::focusedTerminal() const
{
    if (focused_ && focused_->workspace->type() == WorkspaceType::Terminal)
        return &static_cast<TerminalWorkspace&>(*focused_->workspace);
    return nullptr;
}

// View > <type>: closes a tile of that type if there is one (the focused
// one first), else opens one where 1.1 put it: the outline left of the
// editor, the terminal across the bottom, a preview beside the editor.
void App::toggleTile(WorkspaceType type)
{
    Tile* existing = nullptr;
    if (focused_ && focused_->workspace->type() == type)
        existing = focused_;
    for (Tile* tile : tiles_.tiles()) {
        if (!existing && tile->workspace->type() == type)
            existing = tile;
    }
    if (existing) {
        if (tiles_.tileCount() > 1) {
            Action close{Action::Kind::CloseTile};
            close.tile = existing;
            request(close);
        }
        return;
    }

    layout();
    Tile* anchor = editorTile(false);
    if (!anchor)
        anchor = focused_;
    switch (type) {
    case WorkspaceType::Outline:
        tiles_.split(anchor, true, makeWorkspace(type), 0.22f, true);
        break;
    case WorkspaceType::Preview:
        tiles_.split(anchor, true, makeWorkspace(type), 0.5f);
        break;
    case WorkspaceType::Terminal:
        focusTile(tiles_.addAtEdge(Direction::Down, makeWorkspace(type), 0.35f));
        break;
    case WorkspaceType::Editor: {
        const bool horizontal = anchor->rect.w >= anchor->rect.h;
        Tile* tile = tiles_.split(anchor, horizontal, makeWorkspace(type));
        static_cast<EditorWorkspace&>(*tile->workspace).open(std::make_shared<Document>());
        focusTile(tile);
        break;
    }
    }
    updateFocus();
}

// A new tile of the focused one's type: an editor shows the same document
// (a second view of it), a terminal starts another shell.
void App::splitFocused(bool horizontal)
{
    if (!focused_)
        return;
    const WorkspaceType type = focused_->workspace->type();
    std::unique_ptr<Workspace> workspace = makeWorkspace(type);
    if (type == WorkspaceType::Editor)
        static_cast<EditorWorkspace&>(*workspace).open(static_cast<EditorWorkspace&>(*focused_->workspace).sharedDocument());
    Tile* tile = tiles_.split(focused_, horizontal, std::move(workspace));
    focusTile(tile);
}

void App::forEachEditor(const std::function<void(Tile&, EditorWorkspace&, bool)>& fn) const
{
    tiles_.forEachTile([&](Tile& tile) {
        if (tile.workspace->type() == WorkspaceType::Editor)
            fn(tile, static_cast<EditorWorkspace&>(*tile.workspace), true);
        for (const auto& hidden : tile.stash) {
            if (hidden->type() == WorkspaceType::Editor)
                fn(tile, static_cast<EditorWorkspace&>(*hidden), false);
        }
    });
}

// The header's type menu. What the tile showed stays with it, hidden, and
// comes back when its type is chosen again; a new editor shows the current
// document, like a split does.
void App::setTileType(Tile* tile, WorkspaceType type)
{
    if (tile->workspace->type() == type)
        return;
    const std::shared_ptr<Document> doc = currentDocument();
    tile->workspace->setFocused(false);
    tiles_.setType(tile, type, [&](WorkspaceType t) {
        std::unique_ptr<Workspace> workspace = makeWorkspace(t);
        if (t == WorkspaceType::Editor)
            static_cast<EditorWorkspace&>(*workspace).open(doc ? doc : std::make_shared<Document>());
        return workspace;
    });
    if (tile == lastEditor_ && type != WorkspaceType::Editor)
        lastEditor_ = nullptr;
    // Zoom or settings may have changed while it was hidden.
    tile->workspace->styleChanged();
    tile->workspace->imagesLoaded();
    focusTile(tile);
    layout();
    dirty_ = true;
}

void App::openTypeMenu(Tile* tile, bool keyboard)
{
    layout();
    std::vector<MenuItem> items;
    int checked = 0;
    for (size_t i = 0; i < std::size(kWorkspaceTypes); ++i) {
        items.push_back({workspaceTypeLabel(kWorkspaceTypes[i]), "", static_cast<int>(i + 1)});
        if (kWorkspaceTypes[i] == tile->workspace->type())
            checked = static_cast<int>(i + 1);
    }
    typeMenu_->open(tiles_.typeButtonRect(*tile, *text_[kFaceCode]), std::move(items), checked, keyboard);
    typeMenuTile_ = tile;
    dirty_ = true;
}

void App::endTileDrag(bool drop)
{
    if (tileDrag_ == TileDrag::Moving)
        SDL_CaptureMouse(false);
    Tile* source = dragTile_;
    const TileTree::Drop target = drop_;
    tileDrag_ = TileDrag::None;
    dragTile_ = nullptr;
    drop_ = {};
    dirty_ = true;
    if (!drop || !source || target.kind == TileTree::Drop::Kind::None || !tiles_.contains(source))
        return;
    Tile* other = target.kind == TileTree::Drop::Kind::Swap ? target.target : nullptr;
    Tile* moved = tiles_.drop(source, target);
    // A swap trades workspaces between the two places; follow the editor.
    if (other && moved == other) {
        if (lastEditor_ == source)
            lastEditor_ = other;
        else if (lastEditor_ == other)
            lastEditor_ = source;
    }
    focusTile(moved);
    layout();
}

void App::removeTile(Tile* tile)
{
    if (tile == dragTile_)
        endTileDrag(false);
    if (tile == typeMenuTile_) {
        typeMenu_->close();
        typeMenuTile_ = nullptr;
    }
    const bool wasFocused = tile == focused_;
    if (tile == lastEditor_)
        lastEditor_ = nullptr;
    if (focused_ == tile)
        focused_ = nullptr;
    Tile* next = tiles_.remove(tile);
    if (wasFocused || !focused_)
        focusTile(next ? next : (tiles_.root() ? tiles_.tiles().front() : nullptr));
    else
        updateFocus();
}

void App::closeTabNow(Tile* tile, size_t index)
{
    Workspace& ws = *tile->workspace;
    ws.closeTab(index);
    if (ws.tabCount() > 0)
        return;
    // The tile's last tab: the tile goes, unless it's the only one.
    if (tiles_.tileCount() > 1) {
        removeTile(tile);
        return;
    }
    if (ws.type() == WorkspaceType::Editor)
        static_cast<EditorWorkspace&>(ws).open(std::make_shared<Document>());
    else if (ws.type() == WorkspaceType::Terminal)
        static_cast<TerminalWorkspace&>(ws).addShell(workingDirectory());
    updateFocus();
}

void App::followDocuments()
{
    EditorWorkspace* editor = targetEditor();
    if (!editor)
        return;
    const std::shared_ptr<Document> doc = editor->sharedDocument();
    const size_t line = editor->currentSourceLine();
    tiles_.forEachTile([&](Tile& tile) {
        if (tile.workspace->type() == WorkspaceType::Preview)
            static_cast<PreviewWorkspace&>(*tile.workspace).follow(doc);
        else if (tile.workspace->type() == WorkspaceType::Outline)
            static_cast<OutlineWorkspace&>(*tile.workspace).follow(doc, line);
    });
}

// ---------------------------------------------------------------------------
// Layout and drawing
// ---------------------------------------------------------------------------

bool App::loadFonts()
{
    const std::string dir = std::string(SDL_GetBasePath()) + "assets/fonts/";
    const auto load = [&](Face face, const char* file) {
        return fonts_[face].loadFromFile((dir + file).c_str());
    };
    if (!load(kFaceCode, "JetBrainsMono-Regular.ttf") || !load(kFaceRegular, "IBMPlexSerif-Regular.ttf")
        || !load(kFaceBold, "IBMPlexSerif-Bold.ttf") || !load(kFaceItalic, "IBMPlexSerif-Italic.ttf")
        || !load(kFaceBoldItalic, "IBMPlexSerif-BoldItalic.ttf"))
        return false;
    // Headings are the bold face at larger sizes; copies share the file data.
    for (int face = kFaceH1; face <= kFaceH4; ++face)
        fonts_[face] = fonts_[kFaceBold];
    fonts_[kFaceCode].setKerning(false); // monospace: keep every column aligned
    return true;
}

void App::applyFontSize()
{
    const float scale = SDL_GetWindowDisplayScale(window_);
    const float code = std::round(fontPoints_ * scale);
    const float reading = std::round(fontPoints_ * settings_.readingScale * scale);
    fonts_[kFaceCode].setPixelSize(code);
    for (int face = kFaceRegular; face <= kFaceBoldItalic; ++face)
        fonts_[face].setPixelSize(reading);
    for (int i = 0; i < 4; ++i)
        fonts_[kFaceH1 + i].setPixelSize(std::round(reading * kHeadingScale[i]));

    atlas_->reset(); // cached bitmaps are for the old sizes
    ctx_.displayScale = scale;
    layout();
    tiles_.forEachTile([](Tile& tile) { tile.workspace->styleChanged(); });
    dirty_ = true;
}

SDL_FRect App::contentArea() const
{
    int w = 0, h = 0;
    SDL_GetCurrentRenderOutputSize(renderer_, &w, &h);
    const float top = menu_->height();
    const float statusHeight = std::round(static_cast<float>(fonts_[kFaceCode].lineHeight()) * 1.4f);
    return {0.0f, top, static_cast<float>(w), std::max(0.0f, static_cast<float>(h) - top - statusHeight)};
}

void App::layout()
{
    const float tabBar = std::round(static_cast<float>(fonts_[kFaceCode].lineHeight()) * 1.6f);
    tiles_.layout(contentArea(), tabBar);
}

void App::draw()
{
    const Uint64 start = SDL_GetPerformanceCounter();
    atlas_->beginFrame();
    updateMenus();
    followDocuments();
    layout();

    int w = 0, h = 0;
    SDL_GetCurrentRenderOutputSize(renderer_, &w, &h);
    const float width = static_cast<float>(w);
    TextRenderer& uiText = *text_[kFaceCode];
    const float lh = static_cast<float>(uiText.font().lineHeight());

    SDL_SetRenderDrawColorFloat(renderer_, theme::kEditorBg.r, theme::kEditorBg.g, theme::kEditorBg.b, 1.0f);
    SDL_RenderClear(renderer_);

    tiles_.draw(renderer_, uiText, focused_, typeMenuTile_);

    // A tile being dragged: dim it, and shade where it would land.
    if (tileDrag_ == TileDrag::Moving && dragTile_ && tiles_.contains(dragTile_)) {
        ui::fillRect(renderer_, dragTile_->rect, {0.0f, 0.0f, 0.0f, 0.35f});
        if (drop_.kind != TileTree::Drop::Kind::None) {
            const SDL_FRect r = tiles_.dropRect(drop_);
            ui::fillRect(renderer_, r, {theme::kMenuActive.r, theme::kMenuActive.g, theme::kMenuActive.b, 0.25f});
            ui::strokeRect(renderer_, r, theme::kMenuActive);
        }
    }

    // Status bar: the focused tile on the left, the current document on the right.
    const float barHeight = std::round(lh * 1.4f);
    const SDL_FRect bar{0.0f, static_cast<float>(h) - barHeight, width, barHeight};
    ui::fillRect(renderer_, bar, theme::kStatusBg);
    const float pad = std::round(lh * 0.6f);
    const float textY = bar.y + std::round((barHeight - lh) * 0.5f);

    std::string left;
    if (EditorWorkspace* editor = focusedEditor()) {
        if (editor->previewing()) {
            const std::string shortcut = settings_.shortcutLabel(kCmdTogglePreview);
            left = shortcut.empty() ? "Preview" : "Preview (" + shortcut + " to edit)";
        } else {
            char position[64];
            SDL_snprintf(position, sizeof(position), "Ln %zu, Col %zu", editor->editor().cursorLine() + 1,
                         editor->editor().cursorColumn() + 1);
            left = position;
        }
    } else if (focused_) {
        left = focused_->workspace->status();
    }

    char right[160];
    const std::shared_ptr<Document> doc = currentDocument();
    std::string summary;
    if (doc) {
        summary = doc->metaSummary();
        SDL_snprintf(right, sizeof(right), "%s    %zu lines    UTF-8%s    %s    %.2f ms", doc->type().name,
                     doc->buffer().lineCount(), doc->bom() ? " BOM" : "", doc->crlf() ? "CRLF" : "LF", lastFrameMs_);
    } else {
        SDL_snprintf(right, sizeof(right), "%.2f ms", lastFrameMs_);
    }
    uiText.draw(pad, textY, left, theme::kStatusText);
    float statusX = std::round(pad * 3.0f + uiText.measure(left));
    if (!summary.empty()) {
        uiText.draw(statusX, textY, summary, theme::kStatusText);
        statusX = std::round(statusX + pad * 2.0f + uiText.measure(summary));
    }
    if (updater_ && updater_->updateReady())
        uiText.draw(statusX, textY, "Update ready: applies when you close AtlasWriter", theme::kStatusText);
    uiText.draw(std::round(width - pad - uiText.measure(right)), textY, right, theme::kStatusText);
    uiText.flush();

    menu_->draw(renderer_, width);

    // Debug view of the glyph atlas (F1)
    if (showAtlas_) {
        const float side = std::min(512.0f, static_cast<float>(std::min(w, h)) * 0.6f);
        const SDL_FRect dst{width - side - pad, menu_->height() + pad, side, side};
        ui::fillRect(renderer_, dst, {0.0f, 0.0f, 0.0f, 1.0f});
        SDL_RenderTexture(renderer_, atlas_->texture(), nullptr, &dst);
        ui::strokeRect(renderer_, dst, theme::kMenuActive);
    }

    picker_->draw(renderer_, width, menu_->height());
    typeMenu_->draw(renderer_, width, static_cast<float>(h));
    menu_->drawDropdown(renderer_);

    // CPU time to build the frame; Present would include the vsync wait.
    lastFrameMs_ = static_cast<double>(SDL_GetPerformanceCounter() - start) * 1000.0
                 / static_cast<double>(SDL_GetPerformanceFrequency());
    SDL_RenderPresent(renderer_);
    updateTitle();
}

void App::updateTitle()
{
    std::string title;
    if (const std::shared_ptr<Document> doc = currentDocument()) {
        const md::Meta& meta = doc->meta();
        title = meta.title.empty() ? doc->displayName() : meta.title;
        if (doc->modified())
            title += " \xE2\x80\xA2"; // bullet
        title += " - ";
    }
    title += "AtlasWriter";
    if (title != title_) {
        title_ = std::move(title);
        SDL_SetWindowTitle(window_, title_.c_str());
    }
}

void App::updateMouseCursor(float x, float y)
{
    SDL_Cursor* wanted = arrowCursor_;
    TileTree::Divider divider;
    using Part = TileTree::HeaderHit::Part;
    Tile* tile = tiles_.tileAt(x, y);
    const Part part = tile ? tiles_.headerAt(*tile, x, y, *text_[kFaceCode]).part : Part::None;
    if (tileDrag_ == TileDrag::Moving) {
        wanted = moveCursor_;
    } else if (menu_->isOpen() || picker_->isOpen() || typeMenu_->isOpen()) {
        wanted = arrowCursor_;
    } else if (tiles_.dividerAt(x, y, divider)) {
        wanted = tiles_.dividerHorizontal(divider) ? ewResizeCursor_ : nsResizeCursor_;
    } else if (part != Part::None) {
        wanted = part == Part::Type || part == Part::Close ? pointerCursor_ : arrowCursor_;
    } else if (tile) {
        switch (tile->workspace->pointerAt(x, y)) {
        case Workspace::Pointer::Arrow:
            wanted = arrowCursor_;
            break;
        case Workspace::Pointer::Text:
            wanted = textCursor_;
            break;
        case Workspace::Pointer::Hand:
            wanted = pointerCursor_;
            break;
        }
    }
    if (wanted != currentCursor_) {
        currentCursor_ = wanted;
        SDL_SetCursor(wanted);
    }
}

// File and Edit always look the same. View lists every workspace type for
// every file; what doesn't apply to the current one is greyed out.
void App::buildMenus()
{
    const auto item = [&](const char* label, Command command) {
        return MenuItem{label, settings_.shortcutLabel(command), command};
    };
    menu_->clear();
    menu_->addMenu({"File",
                    {
                        item("New", kCmdNew),
                        item("Open...", kCmdOpen),
                        {},
                        item("Save", kCmdSave),
                        item("Save As...", kCmdSaveAs),
                        {},
                        item("Preferences", kCmdPreferences),
                        {},
                        item("Exit", kCmdExit),
                    }});
    menu_->addMenu({"Edit",
                    {
                        item("Undo", kCmdUndo),
                        item("Redo", kCmdRedo),
                        {},
                        item("Cut", kCmdCut),
                        item("Copy", kCmdCopy),
                        item("Paste", kCmdPaste),
                        {},
                        item("Select All", kCmdSelectAll),
                    }});
    menu_->addMenu({"View",
                    {
                        item("Editor", kCmdShowEditor),
                        item("Preview", kCmdShowPreview),
                        item("Outline", kCmdToggleOutline),
                        item("Terminal", kCmdToggleTerminal),
                        {},
                        item("Rendered Markdown", kCmdTogglePreview),
                        item("Collapse All Headings", kCmdCollapseAll),
                        item("Expand All Headings", kCmdExpandAll),
                        {},
                        item("Zoom In", kCmdZoomIn),
                        item("Zoom Out", kCmdZoomOut),
                        item("Reset Zoom", kCmdZoomReset),
                    }});
    menu_->addMenu({"Window",
                    {
                        item("Split Side by Side", kCmdSplitHorizontal),
                        item("Split Stacked", kCmdSplitVertical),
                        item("Close Tile", kCmdCloseTile),
                        item("Change Tile Type...", kCmdChangeTileType),
                        {},
                        item("Next Tab", kCmdNextTab),
                        item("Previous Tab", kCmdPrevTab),
                        item("Close Tab", kCmdCloseTab),
                        {},
                        item("Switch Layout...", kCmdSwitchLayout),
                        item("Save Layout As...", kCmdSaveLayout),
                    }});
    updateMenus();
}

// Enables only what applies right now (checked once per frame; cheap).
void App::updateMenus()
{
    EditorWorkspace* editor = targetEditor();
    const bool editing = editor && !editor->previewing();
    const bool markdown = editor && editor->document().isMarkdown();
    const bool previewTile = focused_ && focused_->workspace->type() == WorkspaceType::Preview;
    menu_->setEnabled(kCmdSave, editor != nullptr);
    menu_->setEnabled(kCmdSaveAs, editor != nullptr);
    menu_->setEnabled(kCmdUndo, editor && editor->editor().canUndo()); // also in the preview (checkboxes)
    menu_->setEnabled(kCmdRedo, editor && editor->editor().canRedo());
    menu_->setEnabled(kCmdCut, editing && editor->editor().hasSelection());
    menu_->setEnabled(kCmdCopy, editing && editor->editor().hasSelection());
    menu_->setEnabled(kCmdPaste, editing);
    menu_->setEnabled(kCmdSelectAll, editing);
    menu_->setEnabled(kCmdTogglePreview, markdown);
    menu_->setEnabled(kCmdCollapseAll, (editor && editor->previewing()) || previewTile);
    menu_->setEnabled(kCmdExpandAll, (editor && editor->previewing()) || previewTile);
    const bool several = tiles_.tileCount() > 1;
    menu_->setEnabled(kCmdCloseTile, several);
    const bool tabs = focused_ && focused_->workspace->tabCount() > 1;
    menu_->setEnabled(kCmdNextTab, tabs);
    menu_->setEnabled(kCmdPrevTab, tabs);
    menu_->setEnabled(kCmdSaveLayout, !settingsPath_.empty());
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

// Reads settings.ccl (writing the defaults there on first run) and applies
// it. On a syntax error the current settings stay; dubious entries are
// skipped and listed.
void App::loadSettings()
{
    if (settingsPath_.empty())
        return;
    std::string text;
    if (readFile(settingsPath_, text)) {
        if (text.starts_with(kUtf8Bom))
            text.erase(0, 3);
    } else {
        text = Settings::defaultText();
        if (!SDL_SaveFile(settingsPath_.c_str(), text.data(), text.size()))
            SDL_Log("Could not write default settings to %s: %s", settingsPath_.c_str(), SDL_GetError());
    }
    settingsTime_ = modifyTime(settingsPath_);

    std::vector<std::string> warnings;
    ccl::Error error;
    if (!Settings::load(text, settings_, warnings, error)) {
        showError("settings.ccl, line " + std::to_string(error.line) + ": " + error.message
                  + "\n\nThe file was not applied; the previous settings are still in use.");
        return;
    }
    if (!warnings.empty()) {
        std::string message = "Some entries in settings.ccl were ignored:\n";
        for (const std::string& w : warnings)
            message += "\n\xE2\x80\xA2 " + w;
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "AtlasWriter", message.c_str(), window_);
    }
    applySettings();
}

void App::applySettings()
{
    fontPoints_ = settings_.fontSize;
    for (auto& text : text_)
        text->setTabSize(settings_.tabSize);
    ctx_.caretBlinkMs = settings_.caretBlinkMs;
    ctx_.shell = settings_.terminalShell;
    ctx_.codeStyle = {false, settings_.codeLineNumbers, settings_.codeHighlightLine, 0.0f};
    ctx_.proseStyle = {true, false, false, settings_.proseColumnEms};
    applyFontSize(); // also re-styles every tile
    buildMenus();
}

// Picks up edits made to settings.ccl outside AtlasWriter.
void App::checkSettingsFile()
{
    if (!settingsPath_.empty() && modifyTime(settingsPath_) != settingsTime_)
        loadSettings();
}

void App::openSettings()
{
    if (settingsPath_.empty()) {
        showError("There is no folder to keep settings in on this system.");
        return;
    }
    if (modifyTime(settingsPath_) == 0) { // deleted since startup
        const std::string text = Settings::defaultText();
        SDL_SaveFile(settingsPath_.c_str(), text.data(), text.size());
    }
    openPath(settingsPath_);
}
