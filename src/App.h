#pragma once

#include <SDL3/SDL.h>

#include "editor/FileType.h"
#include "config/Settings.h"
#include "editor/TextBuffer.h"
#include "text/Font.h"

#include <array>
#include <memory>
#include <string>

class EditorView;
struct EditorStyle;
class GlyphAtlas;
class ImageCache;
class MarkdownView;
class OutlinePanel;
class TerminalView;
class MenuBar;
class TextBatch;
class TextRenderer;
class Updater;

class App
{
public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    bool init(int argc, char* argv[]);
    void run();

private:
    // What to do once unsaved changes have been dealt with.
    enum class Action { None, New, Open, Preferences, Exit };
    enum class SaveResult { Saved, Pending, Failed };

    // Every font/size the app draws with. All share one glyph atlas.
    enum Face : int {
        kFaceCode,       // code workspace and UI
        kFaceRegular,    // reading font: text and markdown workspaces
        kFaceBold,
        kFaceItalic,
        kFaceBoldItalic,
        kFaceH1,
        kFaceH2,
        kFaceH3,
        kFaceH4,
        kFaceCount,
    };

    void handleEvent(SDL_Event& event);
    bool handleShortcut(const SDL_KeyboardEvent& key);
    void runCommand(int command);

    void request(Action action); // asks about unsaved changes first
    void perform(Action action);
    SaveResult save();

    void showOpenDialog();
    void showSaveDialog();
    static void SDLCALL onOpenDialog(void* userdata, const char* const* files, int filter);
    static void SDLCALL onSaveDialog(void* userdata, const char* const* files, int filter);
    void onDialogResult(const SDL_UserEvent& event);

    void newDocument();
    bool loadFile(const char* path);
    bool writeFile(const char* path);
    bool modified() const;
    std::string displayName() const;
    void showError(const std::string& message);

    // Picks the workspace for the current file type: fonts, wrapping,
    // gutter, and whether the Markdown preview is available.
    void applyWorkspace();
    void setPreview(bool preview);
    void documentChanged(); // new path or contents: reset per-document caches
    void updateMenus();

    // Settings (Documents/AtlasWriter/settings.ccl)
    void loadSettings();
    void applySettings();
    void checkSettingsFile();
    void openSettings();
    EditorStyle codeStyle() const;
    EditorStyle proseStyle() const;

    // Workspace panels
    SDL_FRect contentArea() const; // between the menu bar and the status bar
    float minTerminalHeight() const;
    bool outlineShown() const;
    bool terminalShown() const;
    void setTerminalVisible(bool visible);
    void setTerminalFocus(bool focused);
    void updateFocus();
    void goToSourceLine(size_t line);
    void buildMenus();
    std::string baseDirectory() const; // folder of the current file, with a trailing separator
    bool previewing() const { return preview_ && fileType_.kind == FileKind::Markdown; }

    bool loadFonts();
    void applyFontSize();
    void layout();
    void draw();
    void updateTitle();
    void updateMouseCursor(float x, float y);

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Cursor* arrowCursor_ = nullptr;
    SDL_Cursor* textCursor_ = nullptr;
    SDL_Cursor* pointerCursor_ = nullptr;
    SDL_Cursor* resizeCursor_ = nullptr;
    SDL_Cursor* currentCursor_ = nullptr;

    std::array<Font, kFaceCount> fonts_;
    std::unique_ptr<GlyphAtlas> atlas_;
    std::unique_ptr<TextBatch> batch_;
    std::array<std::unique_ptr<TextRenderer>, kFaceCount> text_;
    TextBuffer buffer_;
    std::unique_ptr<EditorView> editor_;
    std::unique_ptr<MarkdownView> markdown_;
    std::unique_ptr<ImageCache> images_;
    std::unique_ptr<OutlinePanel> outline_;
    std::unique_ptr<TerminalView> terminal_;
    std::unique_ptr<MenuBar> menu_;

    // Current document
    std::string path_; // empty for an unsaved new document
    FileType fileType_;
    uint64_t savedState_ = 0; // editor undo state at the last save (see EditorView::stateId)
    bool crlf_ = false; // file used CRLF line endings; restored on save
    bool bom_ = false;  // file started with a UTF-8 BOM; restored on save
    bool preview_ = false; // Markdown: showing the rendered view

    std::unique_ptr<Updater> updater_; // null if update checks are off
    Uint32 updateEvent_ = 0;           // an update is ready to apply
    Uint32 dialogEvent_ = 0; // custom event type for file dialog results
    Uint32 imageEvent_ = 0;  // ... finished image decodes
    Uint32 terminalEvent_ = 0; // ... and terminal output

    bool outlineVisible_ = false;
    bool terminalVisible_ = false;
    bool terminalFocused_ = false; // keyboard goes to the terminal
    bool windowFocused_ = true;
    bool resizingTerminal_ = false;
    float terminalHeight_ = 0.0f; // 0 = default share of the window
    bool dialogOpen_ = false;
    Action pending_ = Action::None; // runs after a Save As dialog completes

    Settings settings_;
    std::string settingsPath_; // empty if there is nowhere to keep it
    bool skipTextInput_ = false; // the last key ran a shortcut
    SDL_Time settingsTime_ = 0; // modification time when last read

    bool running_ = true;
    bool dirty_ = true;
    float fontPoints_ = 15.0f;
    bool showAtlas_ = false;
    double lastFrameMs_ = 0.0;
    std::string title_;
};
