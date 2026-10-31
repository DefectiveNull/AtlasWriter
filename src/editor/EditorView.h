#pragma once

#include "editor/LineWrap.h"
#include "editor/Syntax.h"
#include "editor/UndoHistory.h"
#include "text/TextRenderer.h"

#include <SDL3/SDL.h>

#include <string>
#include <string_view>
#include <vector>

class TextBuffer;
class TextRenderer;

// How an EditorView presents its text; each workspace picks its own.
struct EditorStyle
{
    bool wrap = false;          // soft word wrap at the text column's edge
    bool lineNumbers = true;    // gutter with line numbers
    bool highlightLine = true;  // tint the caret's line
    float maxColumnEms = 0.0f;  // > 0: centered text column at most this many ems wide
};

// Draws a TextBuffer with an optional line-number gutter, caret and
// selection, and turns keyboard/mouse input into edits. Mouse coordinates
// must already be in render (pixel) coordinates.
//
// Everything is addressed in visual rows; without wrapping a row is a line.
// Only the visible rows are laid out and drawn, so cost scales with the
// window size, not the document size.
class EditorView
{
public:
    explicit EditorView(TextBuffer& buffer);

    // Font and presentation. Keeps the caret; re-wraps on next use.
    void setStyle(TextRenderer& text, const EditorStyle& style);
    void invalidateLayout() { layoutDirty_ = true; } // e.g. after a font size change

    void setBounds(const SDL_FRect& bounds) { bounds_ = bounds; }

    // Returns true if the view needs a redraw.
    bool handleEvent(const SDL_Event& event);
    void draw(SDL_Renderer* renderer);

    void reset();       // call after the buffer's contents are replaced
    void revealCaret(); // scroll so the caret is in view

    bool isOverText(float x, float y) const;
    size_t cursorLine() const;   // 0-based
    size_t cursorColumn() const; // 0-based, in codepoints
    size_t topLine();            // first logical line in view

    // Edit commands (the app binds them to shortcuts). Undo/redo return
    // false when there was nothing to do.
    bool undo();
    bool redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }
    void cut();
    void copy() const;
    void paste();
    void selectAll();
    bool hasSelection() const { return cursor_ != anchor_; }

    // Identifies the document state for "unsaved changes" checks: compare
    // with the value taken at save time (see UndoHistory::stateId()).
    uint64_t stateId() const { return history_.stateId(); }
    // Call on save, so the next keystroke starts a new undo step and undo
    // can land exactly on the saved state.
    void breakUndoGroup() { history_.breakGroup(); }

    // Syntax highlighting; null turns it off.
    void setLanguage(const syntax::Language* language);
    // Moves the caret to the start of `line` and scrolls it near the top.
    void goToLine(size_t line);

    void setFocused(bool focused);
    void setCaretBlink(int ms) { blinkMs_ = static_cast<Uint64>(ms < 0 ? 0 : ms); } // 0 = steady
    // Milliseconds until the caret blink changes phase, or -1 if not blinking.
    Sint32 blinkTimeoutMs() const;

private:
    // One visual row: a whole line, or a piece of a wrapped one.
    struct Row
    {
        size_t line;
        size_t start; // absolute byte offsets; end excludes the '\n'
        size_t end;
        bool lastInLine;
    };

    bool handleKey(const SDL_KeyboardEvent& key);

    size_t selectionStart() const { return cursor_ < anchor_ ? cursor_ : anchor_; }
    size_t selectionEnd() const { return cursor_ < anchor_ ? anchor_ : cursor_; }

    void applyEdit(size_t pos, size_t length, std::string_view text);
    void edit(size_t pos, size_t length, std::string_view text, UndoHistory::Kind kind);
    void insert(std::string_view text, UndoHistory::Kind kind);
    void insertNewline();
    void eraseSelection(UndoHistory::Kind kind);

    void moveTo(size_t pos, bool select);
    void moveVertical(long long rows, bool select);
    void selectWord(size_t pos);
    void selectLine(size_t pos);
    size_t wordLeft(size_t pos) const;
    size_t wordRight(size_t pos) const;
    size_t smartHome(size_t pos) const;
    size_t rowHome(size_t pos);
    size_t rowEnd(size_t pos);

    void ensureLayout();
    void colorRuns(size_t from, size_t to);
    size_t rowOf(size_t pos);
    Row rowAt(size_t row);
    std::string_view rowText(const Row& row);
    size_t offsetInRow(const Row& row, float x);

    size_t offsetAt(float x, float y);
    float caretX();
    void clampScroll();
    void resetBlink();
    bool caretVisible() const;

    float lineHeight() const;
    float padding() const;
    float gutterWidth() const;
    SDL_FRect textArea() const;
    float columnLeft() const;  // x of column 0, before scrolling
    float columnWidth() const; // wrap width
    long long visibleRows() const;

    TextBuffer& buffer_;
    TextRenderer* text_ = nullptr;
    EditorStyle style_;
    SDL_FRect bounds_{};

    LineWrap wrap_;
    UndoHistory history_;
    Highlighter highlighter_;
    std::vector<syntax::Span> spans_;              // tokens of the line being drawn
    std::vector<TextRenderer::ColorRun> colorRuns_;
    uint64_t layoutVersion_ = 0; // buffer version the wrap index matches
    bool layoutDirty_ = true;

    size_t cursor_ = 0; // byte offset of the caret
    size_t anchor_ = 0; // other end of the selection (== cursor_ when none)
    float desiredX_ = -1.0f; // x kept across up/down moves; < 0 when unset

    float scrollX_ = 0.0f;
    float scrollY_ = 0.0f;
    bool dragging_ = false;
    bool focused_ = true;
    Uint64 blinkStart_ = 0;
    Uint64 blinkMs_ = 530;

    std::string scratch_; // reused for lines that straddle the buffer gap
    std::vector<size_t> rowStarts_;
};
