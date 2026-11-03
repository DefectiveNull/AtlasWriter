#pragma once

#include "editor/EditorView.h"
#include "markdown/MarkdownView.h"
#include "markdown/OutlinePanel.h"
#include "terminal/TerminalView.h"
#include "workspace/Layout.h"

#include <SDL3/SDL.h>

#include <functional>
#include <memory>
#include <string>

class ImageCache;
class TextRenderer;

// What workspaces draw with. Owned by the App; styles change when the
// settings or the zoom do (see Workspace::styleChanged()).
struct WorkspaceContext
{
    TextRenderer* code = nullptr;    // monospace: code, terminal, UI
    TextRenderer* reading = nullptr; // prose: text and Markdown source
    MarkdownFonts markdown;
    ImageCache* images = nullptr;
    float displayScale = 1.0f;
    EditorStyle codeStyle;
    EditorStyle proseStyle;
    int caretBlinkMs = 530;
    std::string shell; // terminal command; empty = the platform default
    Uint32 terminalEvent = 0;
    // Pushed (from any thread) when a worker or a native view has news:
    // a page rendered, a waveform decoded, a web page loaded. The App then
    // calls onAsync() on every workspace.
    Uint32 asyncEvent = 0;
    SDL_Window* window = nullptr;     // parent of native views (web pages)
    SDL_Renderer* renderer = nullptr; // for textures made outside draw()
    // The open Document for a path, if any (a web tile shows the edited
    // text of an HTML file, not what's on disk).
    std::function<std::shared_ptr<Document>(const std::string& path)> findDocument;
    // Whether a key runs an AtlasWriter command that should still work
    // while a web page has the keyboard (tile keys, tabs; not copy/paste).
    std::function<bool(SDL_Keycode key, SDL_Keymod mod)> isAppShortcut;
};

// The asyncEvent's code when a web page passes on an app shortcut:
// data1 holds the key, data2 the modifiers.
inline constexpr Sint32 kAsyncShortcut = 1;

// The contents of one tile. A workspace has one or more tabs; the tile
// shows a tab bar once there are two.
class Workspace
{
public:
    enum class Pointer { Arrow, Text, Hand };

    explicit Workspace(const WorkspaceContext& context)
        : ctx_(context)
    {
    }
    virtual ~Workspace() = default;
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    virtual WorkspaceType type() const = 0;

    virtual void setBounds(const SDL_FRect& bounds) = 0;
    // Returns true if the tile needs a redraw.
    virtual bool handleEvent(const SDL_Event& event) = 0;
    virtual void draw(SDL_Renderer* renderer) = 0;

    virtual void setFocused(bool) {}
    virtual Pointer pointerAt(float, float) { return Pointer::Arrow; }
    virtual void styleChanged() {}  // fonts, zoom or settings changed
    virtual void imagesLoaded() {}  // decoded images may change Markdown layout
    virtual std::string status() { return {}; } // left of the status bar while focused

    virtual size_t tabCount() const { return 1; }
    virtual size_t activeTab() const { return 0; }
    virtual void selectTab(size_t) {}
    virtual std::string tabTitle(size_t index) const = 0;
    virtual bool tabModified(size_t) const { return false; }
    // Removes a tab without asking anything (the App checks for unsaved
    // changes first). The tile closes once its last tab is gone.
    virtual void closeTab(size_t) {}

    // Drops: the document a file or link dropped on this tile goes into
    // (null: none), where at the point (x, y), and the insertion itself,
    // as one undo step.
    virtual Document* dropDocument() { return nullptr; }
    virtual size_t dropPosition(float, float) { return 0; }
    virtual void insertDropped(size_t, std::string_view) {}

    // File support. A link clicked in the tile, waiting for the App to open
    // it: an absolute path or a URL, once.
    virtual bool takeOpenRequest(std::string&) { return false; }
    // Whether the tile wants to come into focus (its web page was clicked).
    virtual bool takeFocusRequest() { return false; }
    // Opens a file or URL as a tab (reader, web and audio tiles). False if
    // it couldn't be opened; the reason is shown in the tab.
    virtual bool openTarget(const std::string&) { return false; }
    // Something finished on another thread (see WorkspaceContext::asyncEvent).
    // Returns true if the tile needs a redraw.
    virtual bool onAsync() { return false; }
    // Milliseconds until the tile wants drawing again (audio playing), or
    // -1 to wait for events.
    virtual Sint32 wakeMs() const { return -1; }
    // Called every frame. `shown`: the tile shows this workspace (false
    // while it is stashed by a type change); `covered`: a menu, popup or
    // drag preview is open over the window. Native child windows (web
    // pages) only show while shown and not covered; audio stops when hidden.
    virtual void setVisibility(bool /*shown*/, bool /*covered*/) {}
    // The window came back into focus: files may have changed meanwhile.
    virtual void refresh() {}

protected:
    const WorkspaceContext& ctx_;
};

// Tabs of documents. Each tab has its own editor view (caret, scroll) and
// Markdown preview, toggled with Ctrl+E.
class EditorWorkspace final : public Workspace
{
public:
    explicit EditorWorkspace(const WorkspaceContext& context);

    WorkspaceType type() const override { return WorkspaceType::Editor; }
    void setBounds(const SDL_FRect& bounds) override;
    bool handleEvent(const SDL_Event& event) override;
    void draw(SDL_Renderer* renderer) override;
    void setFocused(bool focused) override;
    Pointer pointerAt(float x, float y) override;
    void styleChanged() override;
    void imagesLoaded() override;
    std::string status() override;

    size_t tabCount() const override { return tabs_.size(); }
    size_t activeTab() const override { return active_; }
    void selectTab(size_t index) override;
    std::string tabTitle(size_t index) const override;
    bool tabModified(size_t index) const override;
    void closeTab(size_t index) override;

    // Source view: at the point. Rendered view: after the block there.
    Document* dropDocument() override { return &document(); }
    size_t dropPosition(float x, float y) override;
    void insertDropped(size_t pos, std::string_view text) override;
    bool takeOpenRequest(std::string& target) override;

    // Shows `document`: its tab if there is one, else a new tab after the
    // current one (or in place of an untouched Untitled tab).
    void open(const std::shared_ptr<Document>& document);
    const std::shared_ptr<Document>& documentAt(size_t index) const { return tabs_[index]->doc; }

    // The current tab; there always is one while the tile exists.
    Document& document() const { return *tabs_[active_]->doc; }
    const std::shared_ptr<Document>& sharedDocument() const { return tabs_[active_]->doc; }
    EditorView& editor() const { return *tabs_[active_]->editor; }
    MarkdownView& markdown() const { return *tabs_[active_]->markdown; }

    bool previewing() const { return tabs_[active_]->preview && document().isMarkdown(); }
    void setPreview(bool preview);
    void refreshPreview(); // after an edit made while previewing
    void goToSourceLine(size_t line);
    size_t currentSourceLine();
    Sint32 blinkTimeoutMs() const;

private:
    struct Tab
    {
        std::shared_ptr<Document> doc;
        std::unique_ptr<EditorView> editor;
        std::unique_ptr<MarkdownView> markdown;
        bool preview = false;
        // What the styles were applied for; re-applied when these change
        // (e.g. Save As turns a text file into Markdown).
        FileKind kind = FileKind::Text;
        const char* typeName = nullptr;
        std::string directory;
        bool styled = false;
    };

    void sync(Tab& tab); // applies styles if the document's type or folder changed

    std::vector<std::unique_ptr<Tab>> tabs_; // never moved: a view must not outlive its document
    size_t active_ = 0;
    SDL_FRect bounds_{};
    bool focused_ = false;
};

// Rendered Markdown of the document in the editor used last, or of a
// pinned document.
class PreviewWorkspace final : public Workspace
{
public:
    explicit PreviewWorkspace(const WorkspaceContext& context);

    WorkspaceType type() const override { return WorkspaceType::Preview; }
    void setBounds(const SDL_FRect& bounds) override { view_.setBounds(bounds); }
    bool handleEvent(const SDL_Event& event) override;
    void draw(SDL_Renderer* renderer) override;
    Pointer pointerAt(float x, float y) override;
    void styleChanged() override;
    void imagesLoaded() override { view_.invalidateLayout(); }
    std::string status() override;
    std::string tabTitle(size_t) const override;

    Document* dropDocument() override { return doc_.lock().get(); } // editors keep it alive
    size_t dropPosition(float x, float y) override;
    void insertDropped(size_t pos, std::string_view text) override;
    bool takeOpenRequest(std::string& target) override;

    void follow(const std::shared_ptr<Document>& document);
    const std::shared_ptr<Document> document() const { return doc_.lock(); }
    MarkdownView& view() { return view_; }

private:
    void sync();

    MarkdownView view_;
    std::weak_ptr<Document> doc_;
    const Document* shown_ = nullptr;
    std::string directory_;
    bool styled_ = false;
};

// The headings of the document in the editor used last.
class OutlineWorkspace final : public Workspace
{
public:
    explicit OutlineWorkspace(const WorkspaceContext& context);

    WorkspaceType type() const override { return WorkspaceType::Outline; }
    void setBounds(const SDL_FRect& bounds) override { panel_.setBounds(bounds); }
    bool handleEvent(const SDL_Event& event) override;
    void draw(SDL_Renderer* renderer) override;
    std::string status() override { return "Outline"; }
    std::string tabTitle(size_t) const override;

    void follow(const std::shared_ptr<Document>& document, size_t currentLine);
    // A clicked heading's source line, once.
    bool takeJump(size_t& line);

private:
    OutlinePanel panel_;
    std::weak_ptr<Document> doc_;
    const Document* shown_ = nullptr;
    long long jump_ = -1;
};

// One shell per tab.
class TerminalWorkspace final : public Workspace
{
public:
    TerminalWorkspace(const WorkspaceContext& context, std::string directory);

    WorkspaceType type() const override { return WorkspaceType::Terminal; }
    void setBounds(const SDL_FRect& bounds) override;
    bool handleEvent(const SDL_Event& event) override;
    void draw(SDL_Renderer* renderer) override;
    void setFocused(bool focused) override;
    Pointer pointerAt(float x, float y) override;
    void styleChanged() override;
    std::string status() override { return "Terminal"; }

    size_t tabCount() const override { return shells_.size(); }
    size_t activeTab() const override { return active_; }
    void selectTab(size_t index) override;
    std::string tabTitle(size_t index) const override;
    void closeTab(size_t index) override;

    void addShell(std::string directory);
    // Types `text` into the current shell (a dropped file's path).
    void typeText(const std::string& text);
    bool onOutput(); // the pty event: true if a visible screen changed
    bool hasKeyboard() const;

private:
    struct Shell
    {
        std::unique_ptr<TerminalView> view;
        std::string directory;
        bool started = false;
    };

    std::vector<Shell> shells_;
    size_t active_ = 0;
    SDL_FRect bounds_{};
    bool focused_ = false;
};
