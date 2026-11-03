#include "workspace/Workspace.h"

#include "editor/Syntax.h"
#include "ui/Draw.h"
#include "ui/ImageCache.h"

#include <algorithm>

// ---------------------------------------------------------------------------
// Editor
// ---------------------------------------------------------------------------

EditorWorkspace::EditorWorkspace(const WorkspaceContext& context)
    : Workspace(context)
{
}

void EditorWorkspace::sync(Tab& tab)
{
    const Document& doc = *tab.doc;
    const FileType& type = doc.type();
    if (tab.styled && tab.kind == type.kind && tab.typeName == type.name && tab.directory == doc.assetBase())
        return;
    const bool code = type.kind == FileKind::Code;
    tab.editor->setLanguage(code ? syntax::languageFor(type.name) : nullptr);
    if (code)
        tab.editor->setStyle(*ctx_.code, ctx_.codeStyle);
    else
        tab.editor->setStyle(*ctx_.reading, ctx_.proseStyle);
    tab.editor->setCaretBlink(ctx_.caretBlinkMs);
    if (!tab.styled || tab.directory != doc.assetBase())
        tab.markdown->setImages(ctx_.images, doc.assetBase(), ctx_.displayScale); // also re-lays out
    if (type.kind != FileKind::Markdown)
        tab.preview = false;
    tab.kind = type.kind;
    tab.typeName = type.name;
    tab.directory = doc.assetBase();
    tab.styled = true;
}

void EditorWorkspace::open(const std::shared_ptr<Document>& document)
{
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i]->doc == document) {
            selectTab(i);
            return;
        }
    }
    auto owned = std::make_unique<Tab>();
    Tab& tab = *owned;
    tab.doc = document;
    tab.editor = std::make_unique<EditorView>(*document);
    tab.markdown = std::make_unique<MarkdownView>();
    tab.markdown->setFonts(ctx_.markdown);
    sync(tab);

    // Opening a file replaces an untouched Untitled tab, as the single
    // document window always did.
    if (!tabs_.empty() && tabs_[active_]->doc->pristine() && tabs_[active_]->doc.use_count() == 1) {
        tabs_[active_] = std::move(owned); // the old tab goes whole: its views before its document
    } else {
        const size_t at = tabs_.empty() ? 0 : active_ + 1;
        tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(at), std::move(owned));
        active_ = at;
    }
    setFocused(focused_);
}

void EditorWorkspace::setBounds(const SDL_FRect& bounds)
{
    bounds_ = bounds;
    if (!tabs_.empty()) {
        tabs_[active_]->editor->setBounds(bounds);
        tabs_[active_]->markdown->setBounds(bounds);
    }
}

bool EditorWorkspace::handleEvent(const SDL_Event& event)
{
    Tab& tab = *tabs_[active_];
    sync(tab);
    if (!previewing())
        return tab.editor->handleEvent(event);

    bool redraw = tab.markdown->handleEvent(event);
    size_t pos = 0;
    char mark = ' ';
    if (tab.markdown->takeTaskToggle(pos, mark) && pos < tab.doc->buffer().size()) {
        tab.editor->replace(pos, 1, std::string_view(&mark, 1));
        refreshPreview();
        redraw = true;
    }
    return redraw;
}

void EditorWorkspace::draw(SDL_Renderer* renderer)
{
    Tab& tab = *tabs_[active_];
    sync(tab);
    setBounds(bounds_);
    if (previewing()) {
        // Another tile may have edited the document since the last frame.
        const TextBuffer& buffer = tab.doc->buffer();
        if (tab.markdown->sourceVersion() != buffer.version())
            tab.markdown->setSource(buffer.toString(), buffer.version());
        tab.markdown->draw(renderer);
    } else {
        tab.editor->draw(renderer);
    }
}

void EditorWorkspace::setFocused(bool focused)
{
    focused_ = focused;
    for (size_t i = 0; i < tabs_.size(); ++i)
        tabs_[i]->editor->setFocused(focused && i == active_);
}

Workspace::Pointer EditorWorkspace::pointerAt(float x, float y)
{
    if (previewing())
        return markdown().isOverClickable(x, y) ? Pointer::Hand : Pointer::Arrow;
    return editor().isOverText(x, y) ? Pointer::Text : Pointer::Arrow;
}

void EditorWorkspace::styleChanged()
{
    for (auto& owned : tabs_) {
        Tab& tab = *owned;
        tab.styled = false;
        tab.editor->invalidateLayout();
        tab.markdown->setFonts(ctx_.markdown);
    }
    if (!tabs_.empty()) {
        sync(*tabs_[active_]);
        setBounds(bounds_);
        editor().revealCaret();
    }
}

void EditorWorkspace::imagesLoaded()
{
    for (auto& owned : tabs_)
        owned->markdown->invalidateLayout();
}

std::string EditorWorkspace::status()
{
    return {}; // the App shows the caret position or the preview hint
}

void EditorWorkspace::selectTab(size_t index)
{
    if (index >= tabs_.size())
        return;
    active_ = index;
    sync(*tabs_[active_]);
    setBounds(bounds_);
    setFocused(focused_);
}

std::string EditorWorkspace::tabTitle(size_t index) const
{
    return tabs_[index]->doc->displayName();
}

bool EditorWorkspace::tabModified(size_t index) const
{
    return tabs_[index]->doc->modified();
}

void EditorWorkspace::closeTab(size_t index)
{
    if (index >= tabs_.size())
        return;
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
    if (tabs_.empty()) {
        active_ = 0;
        return;
    }
    if (active_ > index || active_ >= tabs_.size())
        active_ = active_ > 0 ? active_ - 1 : 0;
    selectTab(active_);
}

namespace {

// The offset where source line `line` starts; the end for SIZE_MAX or past the last line.
size_t lineOffset(const TextBuffer& buffer, size_t line)
{
    return line < buffer.lineCount() ? buffer.lineStart(line) : buffer.size();
}

} // namespace

size_t EditorWorkspace::dropPosition(float x, float y)
{
    Tab& tab = *tabs_[active_];
    sync(tab);
    if (previewing())
        return lineOffset(tab.doc->buffer(), tab.markdown->dropLineAt(y));
    return tab.editor->positionAt(x, y);
}

void EditorWorkspace::insertDropped(size_t pos, std::string_view text)
{
    if (previewing()) {
        editor().replace(pos, 0, text); // the caret stays where it was
        refreshPreview();
    } else {
        editor().insertAt(pos, text);
    }
}

void EditorWorkspace::setPreview(bool preview)
{
    Tab& tab = *tabs_[active_];
    if (!tab.doc->isMarkdown() || preview == tab.preview)
        return;
    tab.preview = preview;
    if (preview) {
        // Parsing only happens when the text changed since the last preview.
        const TextBuffer& buffer = tab.doc->buffer();
        tab.markdown->setSource(buffer.toString(), buffer.version());
        tab.markdown->setBounds(bounds_);
        tab.markdown->scrollToSourceLine(tab.editor->topLine());
    } else {
        tab.editor->revealCaret();
    }
}

void EditorWorkspace::refreshPreview()
{
    // Re-parses; scrolling and collapsed headings are kept.
    const TextBuffer& buffer = document().buffer();
    markdown().setSource(buffer.toString(), buffer.version());
}

void EditorWorkspace::goToSourceLine(size_t line)
{
    if (previewing())
        markdown().revealSourceLine(line);
    else
        editor().goToLine(line);
}

size_t EditorWorkspace::currentSourceLine()
{
    return previewing() ? markdown().topSourceLine() : editor().cursorLine();
}

Sint32 EditorWorkspace::blinkTimeoutMs() const
{
    return previewing() ? -1 : editor().blinkTimeoutMs();
}

// ---------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------

PreviewWorkspace::PreviewWorkspace(const WorkspaceContext& context)
    : Workspace(context)
{
    view_.setFonts(ctx_.markdown);
}

void PreviewWorkspace::follow(const std::shared_ptr<Document>& document)
{
    if (document && document.get() != shown_)
        doc_ = document;
}

void PreviewWorkspace::sync()
{
    const std::shared_ptr<Document> doc = doc_.lock();
    if (doc.get() != shown_) {
        shown_ = doc.get();
        view_.invalidateSource();
        view_.expandAll();
        styled_ = false;
    }
    if (!doc) {
        if (view_.sourceVersion() != 0)
            view_.setSource({}, 0);
        return;
    }
    if (!styled_ || directory_ != doc->assetBase()) {
        directory_ = doc->assetBase();
        view_.setImages(ctx_.images, directory_, ctx_.displayScale);
        styled_ = true;
    }
    const TextBuffer& buffer = doc->buffer();
    if (view_.sourceVersion() != buffer.version())
        view_.setSource(buffer.toString(), buffer.version());
}

bool PreviewWorkspace::handleEvent(const SDL_Event& event)
{
    sync();
    bool redraw = view_.handleEvent(event);
    size_t pos = 0;
    char mark = ' ';
    const std::shared_ptr<Document> doc = doc_.lock();
    if (view_.takeTaskToggle(pos, mark) && doc && pos < doc->buffer().size()) {
        doc->edit(pos, 1, std::string_view(&mark, 1));
        sync();
        redraw = true;
    }
    return redraw;
}

void PreviewWorkspace::draw(SDL_Renderer* renderer)
{
    sync();
    view_.draw(renderer);
}

Workspace::Pointer PreviewWorkspace::pointerAt(float x, float y)
{
    return view_.isOverClickable(x, y) ? Pointer::Hand : Pointer::Arrow;
}

void PreviewWorkspace::styleChanged()
{
    view_.setFonts(ctx_.markdown);
    styled_ = false;
    view_.invalidateLayout();
}

std::string PreviewWorkspace::status()
{
    const std::shared_ptr<Document> doc = doc_.lock();
    return doc ? "Preview of " + doc->displayName() : "Preview";
}

size_t PreviewWorkspace::dropPosition(float, float y)
{
    sync();
    const std::shared_ptr<Document> doc = doc_.lock();
    return doc ? lineOffset(doc->buffer(), view_.dropLineAt(y)) : 0;
}

void PreviewWorkspace::insertDropped(size_t pos, std::string_view text)
{
    if (const std::shared_ptr<Document> doc = doc_.lock()) {
        doc->edit(std::min(pos, doc->buffer().size()), 0, text);
        sync();
    }
}

std::string PreviewWorkspace::tabTitle(size_t) const
{
    const std::shared_ptr<Document> doc = doc_.lock();
    return doc ? doc->displayName() : "";
}

// ---------------------------------------------------------------------------
// Outline
// ---------------------------------------------------------------------------

OutlineWorkspace::OutlineWorkspace(const WorkspaceContext& context)
    : Workspace(context)
    , panel_(*context.reading)
{
}

void OutlineWorkspace::follow(const std::shared_ptr<Document>& document, size_t currentLine)
{
    if (document)
        doc_ = document;
    panel_.setCurrentLine(currentLine);
}

bool OutlineWorkspace::handleEvent(const SDL_Event& event)
{
    long long jump = -1;
    const bool redraw = panel_.handleEvent(event, jump);
    if (jump >= 0)
        jump_ = jump;
    return redraw;
}

void OutlineWorkspace::draw(SDL_Renderer* renderer)
{
    static const TextBuffer kEmpty;
    const std::shared_ptr<Document> doc = doc_.lock();
    if (doc.get() != shown_) {
        shown_ = doc.get();
        panel_.invalidate();
    }
    panel_.update(doc ? doc->buffer() : kEmpty); // rescans only after edits
    panel_.draw(renderer);
}

std::string OutlineWorkspace::tabTitle(size_t) const
{
    const std::shared_ptr<Document> doc = doc_.lock();
    return doc ? doc->displayName() : "";
}

bool OutlineWorkspace::takeJump(size_t& line)
{
    if (jump_ < 0)
        return false;
    line = static_cast<size_t>(jump_);
    jump_ = -1;
    return true;
}

// ---------------------------------------------------------------------------
// Terminal
// ---------------------------------------------------------------------------

TerminalWorkspace::TerminalWorkspace(const WorkspaceContext& context, std::string directory)
    : Workspace(context)
{
    addShell(std::move(directory));
}

void TerminalWorkspace::addShell(std::string directory)
{
    Shell shell;
    shell.view = std::make_unique<TerminalView>(*ctx_.code, ctx_.terminalEvent);
    shell.view->setShell(ctx_.shell);
    shell.view->setShowHeader(false); // the tile header names and closes it
    shell.directory = std::move(directory);
    shells_.push_back(std::move(shell));
    active_ = shells_.size() - 1;
    setFocused(focused_);
}

void TerminalWorkspace::setBounds(const SDL_FRect& bounds)
{
    bounds_ = bounds;
    if (!shells_.empty())
        shells_[active_].view->setBounds(bounds);
}

bool TerminalWorkspace::handleEvent(const SDL_Event& event)
{
    return shells_[active_].view->handleEvent(event);
}

void TerminalWorkspace::draw(SDL_Renderer* renderer)
{
    Shell& shell = shells_[active_];
    shell.view->setBounds(bounds_);
    if (!shell.started) {
        shell.view->ensureStarted(shell.directory); // sized to the tile first
        shell.started = true;
    }
    shell.view->draw(renderer);
}

void TerminalWorkspace::setFocused(bool focused)
{
    focused_ = focused;
    for (size_t i = 0; i < shells_.size(); ++i)
        shells_[i].view->setFocused(focused && i == active_);
}

Workspace::Pointer TerminalWorkspace::pointerAt(float x, float y)
{
    return shells_[active_].view->contains(x, y) ? Pointer::Text : Pointer::Arrow;
}

void TerminalWorkspace::styleChanged()
{
    for (Shell& shell : shells_)
        shell.view->setShell(ctx_.shell); // for shells started from now on
}

void TerminalWorkspace::selectTab(size_t index)
{
    if (index >= shells_.size())
        return;
    active_ = index;
    setBounds(bounds_);
    setFocused(focused_);
}

std::string TerminalWorkspace::tabTitle(size_t index) const
{
    const std::string& name = shells_[index].view->shellName();
    std::string title = name.empty() ? "Terminal" : name;
    if (shells_.size() > 1)
        title += " " + std::to_string(index + 1);
    return title;
}

void TerminalWorkspace::closeTab(size_t index)
{
    if (index >= shells_.size())
        return;
    shells_.erase(shells_.begin() + static_cast<std::ptrdiff_t>(index));
    if (shells_.empty()) {
        active_ = 0;
        return;
    }
    if (active_ > index || active_ >= shells_.size())
        active_ = active_ > 0 ? active_ - 1 : 0;
    selectTab(active_);
}

bool TerminalWorkspace::onOutput()
{
    bool changed = false;
    for (size_t i = 0; i < shells_.size(); ++i) {
        const bool c = shells_[i].view->onOutput(); // every shell's output is consumed
        changed |= c && i == active_;
    }
    return changed;
}

void TerminalWorkspace::typeText(const std::string& text)
{
    if (!shells_.empty())
        shells_[active_].view->sendText(text);
}

bool TerminalWorkspace::hasKeyboard() const
{
    return focused_;
}

// ---------------------------------------------------------------------------
// Links clicked in rendered Markdown
// ---------------------------------------------------------------------------

bool EditorWorkspace::takeOpenRequest(std::string& target)
{
    if (tabs_.empty())
        return false;
    MarkdownView& view = *tabs_[active_]->markdown;
    std::string url;
    if (!view.takeLinkClick(url))
        return false;
    target = view.resolveLink(url);
    return !target.empty();
}

bool PreviewWorkspace::takeOpenRequest(std::string& target)
{
    std::string url;
    if (!view_.takeLinkClick(url))
        return false;
    target = view_.resolveLink(url);
    return !target.empty();
}
