#include "App.h"

#include "config/Commands.h"
#include "editor/EditorView.h"
#include "editor/Syntax.h"
#include "markdown/MarkdownView.h"
#include "markdown/OutlinePanel.h"
#include "terminal/TerminalView.h"
#include "text/GlyphAtlas.h"
#include "text/TextRenderer.h"
#include "ui/Draw.h"
#include "ui/ImageCache.h"
#include "ui/Keys.h"
#include "ui/MenuBar.h"
#include "update/Updater.h"

#include <nanosvg.h>
#include <nanosvgrast.h>

#include <algorithm>
#include <cmath>

namespace {

enum DialogKind : Sint32 {
    kDialogOpen = 1,
    kDialogSave,
};

constexpr float kHeadingScale[] = {2.0f, 1.6f, 1.3f, 1.12f}; // h1..h4, relative to the reading font
constexpr const char* kUtf8Bom = "\xEF\xBB\xBF";

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

bool samePath(std::string a, std::string b)
{
    for (std::string* s : {&a, &b}) {
        for (char& c : *s) {
            if (c == '\\')
                c = '/';
#if defined(SDL_PLATFORM_WINDOWS)
            c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
#endif
        }
    }
    return a == b;
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

} // namespace

App::App() = default;

App::~App()
{
    // GPU resources must go before the renderer that owns them.
    updater_.reset();  // cancels any download
    terminal_.reset(); // stops the shell
    outline_.reset();
    editor_.reset();
    markdown_.reset();
    images_.reset(); // joins the decode thread, frees textures
    menu_.reset();
    for (auto& text : text_)
        text.reset();
    batch_.reset();
    atlas_.reset();
    SDL_DestroyCursor(arrowCursor_);
    SDL_DestroyCursor(textCursor_);
    SDL_DestroyCursor(pointerCursor_);
    SDL_DestroyCursor(resizeCursor_);
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

    editor_ = std::make_unique<EditorView>(buffer_);
    markdown_ = std::make_unique<MarkdownView>();
    images_ = std::make_unique<ImageCache>(renderer_, imageEvent_);
    outline_ = std::make_unique<OutlinePanel>(*text_[kFaceRegular]);
    terminal_ = std::make_unique<TerminalView>(*text_[kFaceCode], terminalEvent_);
    MarkdownFonts mdFonts;
    mdFonts.regular = text_[kFaceRegular].get();
    mdFonts.bold = text_[kFaceBold].get();
    mdFonts.italic = text_[kFaceItalic].get();
    mdFonts.boldItalic = text_[kFaceBoldItalic].get();
    mdFonts.code = text_[kFaceCode].get();
    mdFonts.heading[0] = text_[kFaceH1].get();
    mdFonts.heading[1] = text_[kFaceH2].get();
    mdFonts.heading[2] = text_[kFaceH3].get();
    mdFonts.heading[3] = text_[kFaceH4].get();
    mdFonts.heading[4] = text_[kFaceBold].get();
    mdFonts.heading[5] = text_[kFaceBold].get();
    markdown_->setFonts(mdFonts);
    menu_ = std::make_unique<MenuBar>(fonts_[kFaceCode], *text_[kFaceCode]);
    buildMenus();

    arrowCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    textCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    pointerCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    resizeCursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
    currentCursor_ = arrowCursor_;

    editor_->setStyle(*text_[kFaceRegular], proseStyle()); // replaced once a document loads

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
    if (!file || !loadFile(file))
        newDocument();
    setPreview(preview);

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
        const Sint32 timeout = previewing() ? -1 : editor_->blinkTimeoutMs();
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
        // Decoded images change sizes in the rendered view.
        if (images_->processLoaded()) {
            markdown_->invalidateLayout();
            dirty_ |= previewing();
        }
        return;
    }
    if (event.type == terminalEvent_) {
        if (terminal_->onOutput())
            dirty_ |= terminalShown();
        return;
    }

    SDL_ConvertEventToRenderCoordinates(renderer_, &event);

    switch (event.type) {
    case SDL_EVENT_QUIT:
        request(Action::Exit);
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
        if (resizingTerminal_) {
            const SDL_FRect area = contentArea();
            terminalHeight_ = std::clamp(area.y + area.h - event.motion.y, minTerminalHeight(), area.h * 0.85f);
            dirty_ = true;
            return;
        }
        updateMouseCursor(event.motion.x, event.motion.y);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (resizingTerminal_ && event.button.button == SDL_BUTTON_LEFT) {
            resizingTerminal_ = false;
            SDL_CaptureMouse(false);
            return;
        }
        break;
    default:
        break;
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

    // Mouse: clicks pick the panel that gets the keyboard; wheel and motion
    // go to whatever is under the pointer.
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        const float x = event.button.x, y = event.button.y;
        if (terminalShown() && terminal_->contains(x, y)) {
            if (event.button.button == SDL_BUTTON_LEFT && terminal_->isOverCloseButton(x, y)) {
                setTerminalVisible(false);
                return;
            }
            if (event.button.button == SDL_BUTTON_LEFT && terminal_->isOverHeader(x, y)) {
                resizingTerminal_ = true;
                SDL_CaptureMouse(true);
            }
            setTerminalFocus(true);
            dirty_ |= terminal_->handleEvent(event);
            return;
        }
        if (outlineShown() && outline_->contains(x, y)) {
            long long jump = -1;
            dirty_ |= outline_->handleEvent(event, jump);
            if (jump >= 0)
                goToSourceLine(static_cast<size_t>(jump));
            return;
        }
        setTerminalFocus(false);
    }
    if (event.type == SDL_EVENT_MOUSE_WHEEL || event.type == SDL_EVENT_MOUSE_MOTION
        || event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
        long long jump = -1;
        if (outlineShown())
            dirty_ |= outline_->handleEvent(event, jump);
        if (terminalShown())
            dirty_ |= terminal_->handleEvent(event);
        if (event.type == SDL_EVENT_MOUSE_WHEEL
            && ((terminalShown() && terminal_->contains(event.wheel.mouse_x, event.wheel.mouse_y))
                || (outlineShown() && outline_->contains(event.wheel.mouse_x, event.wheel.mouse_y))))
            return;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && terminalShown())
        terminal_->handleEvent(event); // ends a selection drag

    const bool keyboard = event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_TEXT_INPUT;
    if (keyboard && terminalShown() && terminal_->focused()) {
        dirty_ |= terminal_->handleEvent(event);
        return;
    }
    if (previewing())
        dirty_ |= markdown_->handleEvent(event);
    else
        dirty_ |= editor_->handleEvent(event);
}

// Shortcuts come from the settings' keymap. While the terminal has focus,
// the edit commands' keys (Ctrl+C, Ctrl+V, ...) belong to the shell.
bool App::handleShortcut(const SDL_KeyboardEvent& key)
{
    const Command command = settings_.commandFor(key);
    if (command == kCmdNone)
        return false;
    const bool editCommand = command == kCmdUndo || command == kCmdRedo || command == kCmdCut || command == kCmdCopy
                          || command == kCmdPaste || command == kCmdSelectAll;
    if (editCommand && terminalShown() && terminal_->focused())
        return false;
    runCommand(command);
    return true;
}

void App::runCommand(int command)
{
    switch (command) {
    case kCmdNew:
        request(Action::New);
        break;
    case kCmdOpen:
        request(Action::Open);
        break;
    case kCmdSave:
        save();
        break;
    case kCmdSaveAs:
        showSaveDialog();
        break;
    case kCmdPreferences:
        request(Action::Preferences);
        break;
    case kCmdExit:
        request(Action::Exit);
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
        setPreview(!preview_);
        break;
    case kCmdUndo:
        if (!previewing())
            editor_->undo();
        break;
    case kCmdRedo:
        if (!previewing())
            editor_->redo();
        break;
    case kCmdCut:
        if (!previewing())
            editor_->cut();
        break;
    case kCmdCopy:
        if (!previewing())
            editor_->copy();
        break;
    case kCmdPaste:
        if (!previewing())
            editor_->paste();
        break;
    case kCmdSelectAll:
        if (!previewing())
            editor_->selectAll();
        break;
    case kCmdCollapseAll:
        markdown_->collapseAll();
        break;
    case kCmdExpandAll:
        markdown_->expandAll();
        break;
    case kCmdToggleOutline:
        if (fileType_.kind == FileKind::Markdown)
            outlineVisible_ = !outlineVisible_;
        break;
    case kCmdToggleTerminal:
        if (fileType_.kind == FileKind::Code)
            setTerminalVisible(!terminalVisible_);
        break;
    default:
        break;
    }
    dirty_ = true;
}

void App::request(Action action)
{
    if (dialogOpen_)
        return;
    if (!modified()) {
        perform(action);
        return;
    }

    const std::string message = "Do you want to save the changes you made to " + displayName() + "?";
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
        const SaveResult result = save();
        if (result != SaveResult::Pending)
            pending_ = Action::None;
        if (result == SaveResult::Saved)
            perform(action);
    } else if (choice == 2) {
        perform(action);
    }
}

void App::perform(Action action)
{
    switch (action) {
    case Action::New:
        newDocument();
        break;
    case Action::Open:
        showOpenDialog();
        break;
    case Action::Preferences:
        openSettings();
        break;
    case Action::Exit:
        running_ = false;
        break;
    case Action::None:
        break;
    }
}

App::SaveResult App::save()
{
    if (path_.empty()) {
        showSaveDialog();
        return dialogOpen_ ? SaveResult::Pending : SaveResult::Failed;
    }
    return writeFile(path_.c_str()) ? SaveResult::Saved : SaveResult::Failed;
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
    SDL_ShowOpenFileDialog(&App::onOpenDialog, this, window_, fileFilters(), kFileFilterCount,
                           nullptr, false);
}

void App::showSaveDialog()
{
    if (dialogOpen_)
        return;
    dialogOpen_ = true;
    SDL_ShowSaveFileDialog(&App::onSaveDialog, this, window_, fileFilters(), kFileFilterCount,
                           path_.empty() ? nullptr : path_.c_str());
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
    const Action next = pending_;
    pending_ = Action::None;

    if (path && event.code == kDialogOpen) {
        loadFile(path);
    } else if (path && event.code == kDialogSave) {
        if (writeFile(path)) {
            path_ = path;
            fileType_ = detectFileType(path_); // saving as notes.md switches to Markdown
            documentChanged();                 // images now resolve against the new folder
            applyWorkspace();
            perform(next);
        }
    }
    SDL_free(path);
    dirty_ = true;
}

// ---------------------------------------------------------------------------
// Documents
// ---------------------------------------------------------------------------

void App::newDocument()
{
    buffer_.setText("");
    path_.clear();
    fileType_ = {};
    crlf_ = false;
    bom_ = false;
    preview_ = false;
    editor_->reset();
    savedState_ = editor_->stateId();
    documentChanged();
    applyWorkspace();
    dirty_ = true;
}

bool App::loadFile(const char* path)
{
    size_t size = 0;
    void* data = SDL_LoadFile(path, &size);
    if (!data) {
        showError(std::string("Could not open \"") + path + "\":\n" + SDL_GetError());
        return false;
    }

    std::string_view raw(static_cast<const char*>(data), size);
    bom_ = raw.starts_with(kUtf8Bom);
    if (bom_)
        raw.remove_prefix(3);
    bool hadCrlf = false;
    const std::string text = normalizeNewlines(raw, &hadCrlf);
    SDL_free(data);

    buffer_.setText(text);
    crlf_ = hadCrlf;
    path_ = path;
    fileType_ = detectFileType(path_);
    preview_ = false;
    editor_->reset();
    savedState_ = editor_->stateId();
    documentChanged();
    applyWorkspace();
    dirty_ = true;
    return true;
}

bool App::writeFile(const char* path)
{
    const std::string text = buffer_.toString();
    std::string out;
    out.reserve(text.size() + (crlf_ ? buffer_.lineCount() : 0) + 3);
    if (bom_)
        out += kUtf8Bom;
    if (crlf_) {
        for (const char c : text) {
            if (c == '\n')
                out += '\r';
            out += c;
        }
    } else {
        out += text;
    }

    if (!SDL_SaveFile(path, out.data(), out.size())) {
        showError(std::string("Could not save \"") + path + "\":\n" + SDL_GetError());
        return false;
    }
    savedState_ = editor_->stateId();
    editor_->breakUndoGroup(); // so undo can land exactly on the saved text
    if (samePath(path, settingsPath_))
        loadSettings(); // saving settings.ccl applies it
    dirty_ = true;
    return true;
}

std::string App::displayName() const
{
    if (path_.empty())
        return "Untitled";
    const size_t slash = path_.find_last_of("/\\");
    return slash == std::string::npos ? path_ : path_.substr(slash + 1);
}

void App::showError(const std::string& message)
{
    SDL_Log("%s", message.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "AtlasWriter", message.c_str(), window_);
}

// ---------------------------------------------------------------------------
// Workspaces
// ---------------------------------------------------------------------------

void App::applyWorkspace()
{
    const bool code = fileType_.kind == FileKind::Code;
    editor_->setLanguage(code ? syntax::languageFor(fileType_.name) : nullptr);
    if (code)
        editor_->setStyle(*text_[kFaceCode], codeStyle());
    else
        editor_->setStyle(*text_[kFaceRegular], proseStyle());

    buildMenus();
    if (fileType_.kind != FileKind::Markdown)
        preview_ = false;
    updateFocus(); // the terminal only takes keys in the code workspace
    layout();
    editor_->revealCaret();
    dirty_ = true;
}

void App::setPreview(bool preview)
{
    if (fileType_.kind != FileKind::Markdown || preview == preview_)
        return;
    preview_ = preview;
    if (preview_) {
        // Parsing only happens when the text changed since the last preview.
        markdown_->setSource(buffer_.toString(), buffer_.version());
        layout();
        markdown_->scrollToSourceLine(editor_->topLine());
    } else {
        editor_->revealCaret();
    }
    dirty_ = true;
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
    editor_->invalidateLayout();
    markdown_->setImages(images_.get(), baseDirectory(), scale); // also re-lays out
    layout();
    editor_->revealCaret();
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

float App::minTerminalHeight() const
{
    return terminal_->headerHeight() + 3.0f * static_cast<float>(fonts_[kFaceCode].lineHeight());
}

// Splits the area between the document view and the workspace's panels:
// the outline on the left (Markdown), the terminal at the bottom (code).
void App::layout()
{
    SDL_FRect main = contentArea();
    if (outlineShown()) {
        const float em = fonts_[kFaceRegular].pixelSize();
        const float width = std::round(std::clamp(main.w * 0.22f, em * 12.0f, em * 22.0f));
        outline_->setBounds({main.x, main.y, std::min(width, main.w * 0.5f), main.h});
        main.x += outline_->bounds().w;
        main.w -= outline_->bounds().w;
    }
    if (terminalShown()) {
        if (terminalHeight_ <= 0.0f)
            terminalHeight_ = std::round(main.h * 0.35f);
        const float height = std::round(std::clamp(terminalHeight_, std::min(minTerminalHeight(), main.h * 0.85f),
                                                   main.h * 0.85f));
        terminal_->setBounds({main.x, main.y + main.h - height, main.w, height});
        main.h -= height;
    }
    editor_->setBounds(main);
    markdown_->setBounds(main);
}

bool App::outlineShown() const
{
    return outlineVisible_ && fileType_.kind == FileKind::Markdown;
}

bool App::terminalShown() const
{
    return terminalVisible_ && fileType_.kind == FileKind::Code;
}

void App::setTerminalVisible(bool visible)
{
    terminalVisible_ = visible;
    if (visible) {
        layout(); // size the grid before the shell starts
        std::string directory = baseDirectory();
        if (directory.empty()) {
            if (char* cwd = SDL_GetCurrentDirectory()) {
                directory = cwd;
                SDL_free(cwd);
            }
        }
        terminal_->ensureStarted(directory);
    }
    setTerminalFocus(visible);
    dirty_ = true;
}

void App::setTerminalFocus(bool focused)
{
    terminalFocused_ = focused && terminalShown();
    updateFocus();
}

void App::updateFocus()
{
    terminal_->setFocused(windowFocused_ && terminalFocused_ && terminalShown());
    editor_->setFocused(windowFocused_ && !(terminalFocused_ && terminalShown()));
    dirty_ = true;
}

// Outline click: scroll whichever view is showing to that heading.
void App::goToSourceLine(size_t line)
{
    if (previewing())
        markdown_->revealSourceLine(line);
    else
        editor_->goToLine(line);
    dirty_ = true;
}

void App::draw()
{
    const Uint64 start = SDL_GetPerformanceCounter();
    atlas_->beginFrame();
    updateMenus();
    layout();

    int w = 0, h = 0;
    SDL_GetCurrentRenderOutputSize(renderer_, &w, &h);
    const float width = static_cast<float>(w);
    TextRenderer& uiText = *text_[kFaceCode];
    const float lh = static_cast<float>(uiText.font().lineHeight());

    SDL_SetRenderDrawColorFloat(renderer_, theme::kEditorBg.r, theme::kEditorBg.g, theme::kEditorBg.b, 1.0f);
    SDL_RenderClear(renderer_);

    if (previewing())
        markdown_->draw(renderer_);
    else
        editor_->draw(renderer_);
    if (outlineShown()) {
        outline_->update(buffer_); // rescans only after edits
        outline_->setCurrentLine(previewing() ? markdown_->topSourceLine() : editor_->cursorLine());
        outline_->draw(renderer_);
    }
    if (terminalShown())
        terminal_->draw(renderer_);

    // Status bar
    const float barHeight = std::round(lh * 1.4f);
    const SDL_FRect bar{0.0f, static_cast<float>(h) - barHeight, width, barHeight};
    ui::fillRect(renderer_, bar, theme::kStatusBg);
    const float pad = std::round(lh * 0.6f);
    const float textY = bar.y + std::round((barHeight - lh) * 0.5f);

    char left[64];
    if (previewing()) {
        const std::string shortcut = settings_.shortcutLabel(kCmdTogglePreview);
        if (shortcut.empty())
            SDL_strlcpy(left, "Preview", sizeof(left));
        else
            SDL_snprintf(left, sizeof(left), "Preview (%s to edit)", shortcut.c_str());
    } else
        SDL_snprintf(left, sizeof(left), "Ln %zu, Col %zu", editor_->cursorLine() + 1, editor_->cursorColumn() + 1);
    char right[160];
    SDL_snprintf(right, sizeof(right), "%s    %zu lines    UTF-8%s    %s    %.2f ms", fileType_.name,
                 buffer_.lineCount(), bom_ ? " BOM" : "", crlf_ ? "CRLF" : "LF", lastFrameMs_);
    uiText.draw(pad, textY, left, theme::kStatusText);
    if (updater_ && updater_->updateReady()) {
        uiText.draw(std::round(pad * 3.0f + uiText.measure(left)), textY,
                    "Update ready: applies when you close AtlasWriter", theme::kStatusText);
    }
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

    menu_->drawDropdown(renderer_);

    // CPU time to build the frame; Present would include the vsync wait.
    lastFrameMs_ = static_cast<double>(SDL_GetPerformanceCounter() - start) * 1000.0
                 / static_cast<double>(SDL_GetPerformanceFrequency());
    SDL_RenderPresent(renderer_);
    updateTitle();
}

void App::updateTitle()
{
    std::string title = displayName();
    if (modified())
        title += " \xE2\x80\xA2"; // bullet
    title += " - AtlasWriter";
    if (title != title_) {
        title_ = std::move(title);
        SDL_SetWindowTitle(window_, title_.c_str());
    }
}

void App::updateMouseCursor(float x, float y)
{
    SDL_Cursor* wanted = arrowCursor_;
    if (!menu_->isOpen()) {
        if (terminalShown() && terminal_->contains(x, y)) {
            if (terminal_->isOverCloseButton(x, y))
                wanted = pointerCursor_;
            else if (terminal_->isOverHeader(x, y))
                wanted = resizeCursor_;
            else
                wanted = textCursor_;
        } else if (outlineShown() && outline_->contains(x, y)) {
            wanted = arrowCursor_;
        } else if (previewing()) {
            wanted = markdown_->isOverHeading(x, y) ? pointerCursor_ : arrowCursor_;
        } else if (editor_->isOverText(x, y)) {
            wanted = textCursor_;
        }
    }
    if (wanted != currentCursor_) {
        currentCursor_ = wanted;
        SDL_SetCursor(wanted);
    }
}

bool App::modified() const
{
    return editor_->stateId() != savedState_;
}

std::string App::baseDirectory() const
{
    const size_t slash = path_.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path_.substr(0, slash + 1);
}

void App::documentChanged()
{
    images_->clear();
    markdown_->expandAll();
    markdown_->setImages(images_.get(), baseDirectory(), SDL_GetWindowDisplayScale(window_));
}

// The File and Edit menus are always there; View only lists what the
// current workspace offers (and disappears when that is nothing).
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

    std::vector<MenuItem> view;
    switch (fileType_.kind) {
    case FileKind::Markdown:
        view = {
            item("Rendered Markdown", kCmdTogglePreview),
            item("Outline", kCmdToggleOutline),
            {},
            item("Collapse All Headings", kCmdCollapseAll),
            item("Expand All Headings", kCmdExpandAll),
        };
        break;
    case FileKind::Code:
        view = {item("Terminal", kCmdToggleTerminal)};
        break;
    case FileKind::Text:
        break;
    }
    if (!view.empty()) {
        view.push_back({});
        view.push_back(item("Zoom In", kCmdZoomIn));
        view.push_back(item("Zoom Out", kCmdZoomOut));
        view.push_back(item("Reset Zoom", kCmdZoomReset));
        menu_->addMenu({"View", std::move(view)});
    }
    updateMenus();
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
    size_t size = 0;
    if (void* data = SDL_LoadFile(settingsPath_.c_str(), &size)) {
        text.assign(static_cast<const char*>(data), size);
        SDL_free(data);
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
    editor_->setCaretBlink(settings_.caretBlinkMs);
    terminal_->setShell(settings_.terminalShell);
    applyFontSize();
    applyWorkspace(); // editor styles and menus
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
    loadFile(settingsPath_.c_str());
}

EditorStyle App::codeStyle() const
{
    return {false, settings_.codeLineNumbers, settings_.codeHighlightLine, 0.0f};
}

EditorStyle App::proseStyle() const
{
    return {true, false, false, settings_.proseColumnEms};
}
// Enables only what applies right now (checked once per frame; cheap).
void App::updateMenus()
{
    const bool editing = !previewing();
    menu_->setEnabled(kCmdUndo, editing && editor_->canUndo());
    menu_->setEnabled(kCmdRedo, editing && editor_->canRedo());
    menu_->setEnabled(kCmdCut, editing && editor_->hasSelection());
    menu_->setEnabled(kCmdCopy, editing && editor_->hasSelection());
    menu_->setEnabled(kCmdPaste, editing);
    menu_->setEnabled(kCmdSelectAll, editing);
    menu_->setEnabled(kCmdTogglePreview, fileType_.kind == FileKind::Markdown);
    menu_->setEnabled(kCmdCollapseAll, previewing());
    menu_->setEnabled(kCmdExpandAll, previewing());
}
