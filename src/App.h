#pragma once

#include <SDL3/SDL.h>

#include "config/Settings.h"
#include "text/Font.h"
#include "workspace/TileTree.h"
#include "workspace/Workspace.h"

#include <array>
#include <deque>
#include <memory>
#include <string>

class GlyphAtlas;
class ImageCache;
class MenuBar;
class Picker;
class PopupMenu;
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
    using Tile = TileTree::Node;

    // Something that can drop documents with unsaved changes, so it runs
    // once those have been dealt with (see request()).
    struct Action
    {
        enum class Kind { None, Exit, CloseTab, CloseTile, SwitchLayout } kind = Kind::None;
        Tile* tile = nullptr; // CloseTab, CloseTile
        size_t tab = 0;       // CloseTab
        std::string layout;   // SwitchLayout
    };
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
    void handleMouse(const SDL_Event& event);
    bool handleShortcut(const SDL_KeyboardEvent& key);
    void runCommand(int command);
    void afterTileEvent(Tile& tile); // outline jumps

    // Unsaved changes: asks once about every modified document `action`
    // would drop, saves the ones the user wants kept, then performs it.
    void request(const Action& action);
    void perform(const Action& action);
    std::vector<std::shared_ptr<Document>> droppedDocuments(const Action& action) const;
    void continueSaving(); // works through saveQueue_, then runs pending_
    SaveResult save(const std::shared_ptr<Document>& document);
    bool writeDocument(Document& document, const std::string& path);

    void showOpenDialog();
    void showSaveDialog(const std::shared_ptr<Document>& document);
    static void SDLCALL onOpenDialog(void* userdata, const char* const* files, int filter);
    static void SDLCALL onSaveDialog(void* userdata, const char* const* files, int filter);
    void onDialogResult(const SDL_UserEvent& event);

    // Documents and routing
    void newDocument();
    void openPath(const std::string& path); // its tab if open, else a new tab in the editor used last
    // File support: where a file or URL opens. Websites and local HTML go to
    // a web tile, PDFs and EPUBs to a reader, audio to an audio tile, text to
    // the editor, anything else to the system. `source` is the tile it was
    // clicked in (a new tile splits it); `rendered` opens Markdown rendered.
    void openTarget(const std::string& target, Tile* source = nullptr, bool rendered = false);
    // A tile of `type`: the focused one if it is, else the first one, else
    // a new one splitting `source` (or the focused tile) along its longer side.
    Tile* tileFor(WorkspaceType type, Tile* source);
    void onAsync(const SDL_UserEvent& event); // see WorkspaceContext::asyncEvent
    std::shared_ptr<Document> currentDocument() const;
    void showError(const std::string& message);

    // Tiles
    std::unique_ptr<Workspace> makeWorkspace(WorkspaceType type);
    std::unique_ptr<Tile> buildTile(const LayoutNode& node, std::vector<Tile*>& editors);
    void applyLayout(const Layout& layout, const std::vector<std::shared_ptr<Document>>& documents,
                     const std::shared_ptr<Document>& active);
    void switchLayout(const std::string& name);
    void saveLayoutAs(const std::string& name);
    void focusTile(Tile* tile);
    void updateFocus();
    Tile* editorTile(bool create); // the editor tile used last (creating one if asked)
    EditorWorkspace* targetEditor() const; // the focused editor, else the one used last
    EditorWorkspace* focusedEditor() const;
    TerminalWorkspace* focusedTerminal() const;
    void toggleTile(WorkspaceType type);
    void splitFocused(bool horizontal);
    void removeTile(Tile* tile);
    void closeTabNow(Tile* tile, size_t index);
    void onPicked(int id, const std::string& choice);
    // Every editor, hidden ones (stashed by a type change) too.
    void forEachEditor(const std::function<void(Tile& tile, EditorWorkspace& editor, bool visible)>& fn) const;
    void setTileType(Tile* tile, WorkspaceType type);
    void openTypeMenu(Tile* tile, bool keyboard);
    void endTileDrag(bool drop);
    // Files and text dropped on the window, inserted once the drop completes.
    void handleDrop(float x, float y);
    void followDocuments(); // preview and outline tiles show the current document

    // Settings (Documents/AtlasWriter/settings.ccl)
    void loadSettings();
    void applySettings();
    void checkSettingsFile();
    void openSettings();
    void buildMenus();
    void updateMenus();

    bool loadFonts();
    void applyFontSize();
    SDL_FRect contentArea() const; // between the menu bar and the status bar
    void layout();
    void draw();
    void updateTitle();
    void updateMouseCursor(float x, float y);

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Cursor* arrowCursor_ = nullptr;
    SDL_Cursor* textCursor_ = nullptr;
    SDL_Cursor* pointerCursor_ = nullptr;
    SDL_Cursor* nsResizeCursor_ = nullptr;
    SDL_Cursor* ewResizeCursor_ = nullptr;
    SDL_Cursor* moveCursor_ = nullptr;
    SDL_Cursor* currentCursor_ = nullptr;

    std::array<Font, kFaceCount> fonts_;
    std::unique_ptr<GlyphAtlas> atlas_;
    std::unique_ptr<TextBatch> batch_;
    std::array<std::unique_ptr<TextRenderer>, kFaceCount> text_;
    std::unique_ptr<ImageCache> images_;
    std::unique_ptr<MenuBar> menu_;
    std::unique_ptr<Picker> picker_;
    std::unique_ptr<PopupMenu> typeMenu_;
    Tile* typeMenuTile_ = nullptr; // whose type menu is open

    WorkspaceContext ctx_;
    TileTree tiles_;
    Tile* focused_ = nullptr;    // gets the keyboard
    Tile* lastEditor_ = nullptr; // where files open; what previews and outlines follow
    bool draggingDivider_ = false;
    TileTree::Divider divider_;
    // Dragging a tile by its header: pending until the pointer moves a few pixels.
    enum class TileDrag { None, Pending, Moving } tileDrag_ = TileDrag::None;
    Tile* dragTile_ = nullptr;
    float dragStartX_ = 0.0f, dragStartY_ = 0.0f;
    TileTree::Drop drop_;
    struct DroppedItem
    {
        bool file; // a path; else dropped text (a link from a browser, ...)
        std::string data;
    };
    std::vector<DroppedItem> dropped_; // between DROP_BEGIN and DROP_COMPLETE

    std::unique_ptr<Updater> updater_; // null if update checks are off
    Uint32 updateEvent_ = 0;           // an update is ready to apply
    Uint32 dialogEvent_ = 0; // custom event type for file dialog results
    Uint32 imageEvent_ = 0;  // ... finished image decodes
    Uint32 terminalEvent_ = 0; // ... and terminal output
    Uint32 asyncEvent_ = 0;    // ... and workers and web pages (WorkspaceContext::asyncEvent)
    Tile* addressTile_ = nullptr; // the web tile whose address is being typed

    bool windowFocused_ = true;
    bool dialogOpen_ = false;
    std::shared_ptr<Document> dialogDocument_;           // being saved by the Save As dialog
    std::deque<std::shared_ptr<Document>> saveQueue_;     // to save before pending_ runs
    Action pending_;

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
