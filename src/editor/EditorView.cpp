#include "EditorView.h"

#include "editor/TextBuffer.h"
#include "text/Font.h"
#include "text/TextRenderer.h"
#include "text/Utf8.h"
#include "ui/Draw.h"
#include "ui/Keys.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace {

constexpr float kWheelLines = 3.0f;

enum class CharClass { Space, Word, Punct, Newline };

CharClass classify(char c)
{
    const auto u = static_cast<unsigned char>(c);
    if (c == '\n')
        return CharClass::Newline;
    if (c == ' ' || c == '\t')
        return CharClass::Space;
    // Bytes >= 0x80 are parts of non-ASCII codepoints: treat them as letters.
    if (u >= 0x80 || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return CharClass::Word;
    return CharClass::Punct;
}

} // namespace

EditorView::EditorView(TextBuffer& buffer)
    : buffer_(buffer)
    , wrap_(buffer)
    , highlighter_(buffer)
{
    resetBlink();
}

void EditorView::setStyle(TextRenderer& text, const EditorStyle& style)
{
    text_ = &text;
    style_ = style;
    layoutDirty_ = true;
    desiredX_ = -1.0f;
    if (style_.wrap)
        scrollX_ = 0.0f;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

bool EditorView::handleEvent(const SDL_Event& event)
{
    ensureLayout();

    switch (event.type) {
    case SDL_EVENT_TEXT_INPUT: {
        const auto first = static_cast<unsigned char>(event.text.text[0]);
        if (first < 0x20 || first == 0x7F) // control chars are handled as keys
            return false;
        insert(event.text.text, UndoHistory::Kind::Typing);
        return true;
    }

    case SDL_EVENT_KEY_DOWN:
        return handleKey(event.key);

    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (event.button.button != SDL_BUTTON_LEFT || !ui::contains(bounds_, event.button.x, event.button.y))
            return false;
        const size_t pos = offsetAt(event.button.x, event.button.y);
        if (event.button.clicks >= 3)
            selectLine(pos);
        else if (event.button.clicks == 2)
            selectWord(pos);
        else
            moveTo(pos, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
        dragging_ = true;
        SDL_CaptureMouse(true); // keep receiving motion when dragging outside the window
        resetBlink();
        return true;
    }

    case SDL_EVENT_MOUSE_MOTION:
        if (!dragging_)
            return false;
        cursor_ = offsetAt(event.motion.x, event.motion.y);
        desiredX_ = -1.0f;
        revealCaret();
        resetBlink();
        return true;

    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button == SDL_BUTTON_LEFT && dragging_) {
            dragging_ = false;
            SDL_CaptureMouse(false);
        }
        return false;

    case SDL_EVENT_MOUSE_WHEEL: {
        if (!ui::contains(bounds_, event.wheel.mouse_x, event.wheel.mouse_y))
            return false;
        float dx = event.wheel.x;
        float dy = event.wheel.y;
        if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            dx = -dx;
            dy = -dy;
        }
        if ((SDL_GetModState() & SDL_KMOD_SHIFT) && dx == 0.0f) {
            dx = -dy;
            dy = 0.0f;
        }
        scrollY_ -= dy * kWheelLines * lineHeight();
        if (!style_.wrap)
            scrollX_ += dx * kWheelLines * text_->advance(' ');
        clampScroll();
        return true;
    }

    default:
        return false;
    }
}

bool EditorView::handleKey(const SDL_KeyboardEvent& key)
{
    const bool shift = (key.mod & SDL_KMOD_SHIFT) != 0;
    const bool word = keys::word(key.mod);
    const bool primary = keys::primary(key.mod);

    switch (keys::normalized(key.key)) {
    case SDLK_LEFT:
        if (hasSelection() && !shift)
            moveTo(selectionStart(), false);
        else
            moveTo(word ? wordLeft(cursor_) : buffer_.prevCodepoint(cursor_), shift);
        break;
    case SDLK_RIGHT:
        if (hasSelection() && !shift)
            moveTo(selectionEnd(), false);
        else
            moveTo(word ? wordRight(cursor_) : buffer_.nextCodepoint(cursor_), shift);
        break;
    case SDLK_UP:
        moveVertical(-1, shift);
        break;
    case SDLK_DOWN:
        moveVertical(1, shift);
        break;
    case SDLK_PAGEUP:
        scrollY_ -= static_cast<float>(visibleRows()) * lineHeight();
        moveVertical(-visibleRows(), shift);
        break;
    case SDLK_PAGEDOWN:
        scrollY_ += static_cast<float>(visibleRows()) * lineHeight();
        moveVertical(visibleRows(), shift);
        break;
    case SDLK_HOME:
        moveTo(primary ? 0 : rowHome(cursor_), shift);
        break;
    case SDLK_END:
        moveTo(primary ? buffer_.size() : rowEnd(cursor_), shift);
        break;
    case SDLK_BACKSPACE:
        if (hasSelection()) {
            eraseSelection(UndoHistory::Kind::Other);
        } else {
            const size_t from = word ? wordLeft(cursor_) : buffer_.prevCodepoint(cursor_);
            if (from < cursor_)
                edit(from, cursor_ - from, {}, UndoHistory::Kind::Backspace);
        }
        break;
    case SDLK_DELETE:
        if (hasSelection()) {
            eraseSelection(UndoHistory::Kind::Other);
        } else {
            const size_t to = word ? wordRight(cursor_) : buffer_.nextCodepoint(cursor_);
            if (to > cursor_)
                edit(cursor_, to - cursor_, {}, UndoHistory::Kind::Delete);
        }
        break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        insertNewline();
        break;
    case SDLK_TAB:
        insert("\t", UndoHistory::Kind::Typing);
        break;
    default:
        return false;
    }

    revealCaret();
    resetBlink();
    return true;
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

// Every buffer change goes through here so the wrap index can re-wrap just
// the lines that changed instead of the whole document.
void EditorView::applyEdit(size_t pos, size_t length, std::string_view text)
{
    const bool layoutCurrent = !layoutDirty_ && layoutVersion_ == buffer_.version();
    const size_t first = buffer_.lineOf(pos);
    const size_t oldLast = buffer_.lineOf(pos + length);

    if (length > 0)
        buffer_.erase(pos, length);
    if (!text.empty())
        buffer_.insert(pos, text);

    highlighter_.linesChanged(first, oldLast - first + 1, buffer_.lineOf(pos + text.size()) - first + 1);
    if (layoutCurrent) {
        const size_t newLast = buffer_.lineOf(pos + text.size());
        wrap_.update(first, oldLast - first + 1, newLast - first + 1);
        layoutVersion_ = buffer_.version();
    }
}

// A user edit: applied, recorded for undo, caret placed after the new text.
void EditorView::edit(size_t pos, size_t length, std::string_view text, UndoHistory::Kind kind)
{
    const UndoHistory::Selection before{cursor_, anchor_};
    std::string removed = buffer_.substr(pos, length);
    applyEdit(pos, length, text);
    cursor_ = anchor_ = pos + text.size();
    desiredX_ = -1.0f;
    history_.record(kind, {pos, std::move(removed), std::string(text)}, before, {cursor_, anchor_}, SDL_GetTicks());
}

void EditorView::insert(std::string_view text, UndoHistory::Kind kind)
{
    const size_t start = selectionStart();
    edit(start, selectionEnd() - start, text, kind);
    revealCaret();
    resetBlink();
}

void EditorView::insertNewline()
{
    // Carry the current line's indentation over to the new line.
    const size_t start = buffer_.lineStart(buffer_.lineOf(selectionStart()));
    size_t indentEnd = start;
    while (indentEnd < selectionStart() && classify(buffer_.at(indentEnd)) == CharClass::Space)
        ++indentEnd;
    insert("\n" + buffer_.substr(start, indentEnd - start), UndoHistory::Kind::Other);
}

void EditorView::eraseSelection(UndoHistory::Kind kind)
{
    if (!hasSelection())
        return;
    const size_t start = selectionStart();
    edit(start, selectionEnd() - start, {}, kind);
}

bool EditorView::undo()
{
    if (!history_.canUndo())
        return false;
    const UndoHistory::Group& group = history_.peekUndo();
    for (auto it = group.edits.rbegin(); it != group.edits.rend(); ++it)
        applyEdit(it->pos, it->inserted.size(), it->removed);
    cursor_ = group.before.cursor;
    anchor_ = group.before.anchor;
    history_.commitUndo();
    desiredX_ = -1.0f;
    revealCaret();
    resetBlink();
    return true;
}

bool EditorView::redo()
{
    if (!history_.canRedo())
        return false;
    const UndoHistory::Group& group = history_.peekRedo();
    for (const UndoHistory::Edit& e : group.edits)
        applyEdit(e.pos, e.removed.size(), e.inserted);
    cursor_ = group.after.cursor;
    anchor_ = group.after.anchor;
    history_.commitRedo();
    desiredX_ = -1.0f;
    revealCaret();
    resetBlink();
    return true;
}

void EditorView::selectAll()
{
    anchor_ = 0;
    cursor_ = buffer_.size();
    desiredX_ = -1.0f;
    history_.breakGroup();
}

void EditorView::copy() const
{
    if (!hasSelection())
        return;
    const std::string text = buffer_.substr(selectionStart(), selectionEnd() - selectionStart());
    SDL_SetClipboardText(text.c_str());
}

void EditorView::cut()
{
    copy();
    eraseSelection(UndoHistory::Kind::Other);
    revealCaret();
    resetBlink();
}

void EditorView::paste()
{
    char* clipboard = SDL_GetClipboardText();
    if (!clipboard)
        return;
    const std::string text = normalizeNewlines(clipboard);
    SDL_free(clipboard);
    if (!text.empty())
        insert(text, UndoHistory::Kind::Other);
}

// ---------------------------------------------------------------------------
// Cursor movement
// ---------------------------------------------------------------------------

void EditorView::moveTo(size_t pos, bool select)
{
    cursor_ = pos;
    if (!select)
        anchor_ = pos;
    desiredX_ = -1.0f;
    history_.breakGroup(); // typing after moving the caret is a new undo step
}

void EditorView::moveVertical(long long rows, bool select)
{
    if (desiredX_ < 0.0f)
        desiredX_ = caretX();
    const float keepX = desiredX_;

    const long long target = static_cast<long long>(rowOf(cursor_)) + rows;
    size_t pos = 0;
    if (target < 0)
        pos = 0;
    else if (target >= static_cast<long long>(wrap_.rowCount()))
        pos = buffer_.size();
    else
        pos = offsetInRow(rowAt(static_cast<size_t>(target)), keepX);
    moveTo(pos, select);
    desiredX_ = keepX;
}

void EditorView::selectWord(size_t pos)
{
    const size_t n = buffer_.size();
    size_t probe = pos;
    if (probe >= n || buffer_.at(probe) == '\n')
        probe = pos > 0 ? pos - 1 : pos;
    if (n == 0 || buffer_.at(probe) == '\n') {
        moveTo(pos, false);
        return;
    }

    const CharClass cls = classify(buffer_.at(probe));
    size_t start = probe;
    size_t end = probe + 1;
    while (start > 0 && classify(buffer_.at(start - 1)) == cls)
        --start;
    while (end < n && classify(buffer_.at(end)) == cls)
        ++end;
    anchor_ = start;
    cursor_ = end;
    desiredX_ = -1.0f;
}

void EditorView::selectLine(size_t pos)
{
    const size_t line = buffer_.lineOf(pos);
    anchor_ = buffer_.lineStart(line);
    cursor_ = line + 1 < buffer_.lineCount() ? buffer_.lineStart(line + 1) : buffer_.size();
    desiredX_ = -1.0f;
}

size_t EditorView::wordLeft(size_t pos) const
{
    if (pos > 0 && buffer_.at(pos - 1) == '\n')
        return pos - 1;
    while (pos > 0 && classify(buffer_.at(pos - 1)) == CharClass::Space)
        --pos;
    if (pos == 0)
        return 0;
    const CharClass cls = classify(buffer_.at(pos - 1));
    if (cls == CharClass::Newline)
        return pos;
    while (pos > 0 && classify(buffer_.at(pos - 1)) == cls)
        --pos;
    return pos;
}

size_t EditorView::wordRight(size_t pos) const
{
    const size_t n = buffer_.size();
    if (pos < n && buffer_.at(pos) == '\n')
        return pos + 1;
    while (pos < n && classify(buffer_.at(pos)) == CharClass::Space)
        ++pos;
    if (pos >= n)
        return n;
    const CharClass cls = classify(buffer_.at(pos));
    if (cls == CharClass::Newline)
        return pos;
    while (pos < n && classify(buffer_.at(pos)) == cls)
        ++pos;
    return pos;
}

size_t EditorView::smartHome(size_t pos) const
{
    // First press goes to the first non-blank character, second to column 0.
    const size_t line = buffer_.lineOf(pos);
    const size_t start = buffer_.lineStart(line);
    const size_t end = buffer_.lineEnd(line);
    size_t firstText = start;
    while (firstText < end && classify(buffer_.at(firstText)) == CharClass::Space)
        ++firstText;
    return pos == firstText ? start : firstText;
}

size_t EditorView::rowHome(size_t pos)
{
    // On a continuation row, Home goes to the row start first.
    const Row row = rowAt(rowOf(pos));
    if (row.start != buffer_.lineStart(row.line) && pos != row.start)
        return row.start;
    return smartHome(pos);
}

size_t EditorView::rowEnd(size_t pos)
{
    // A row's end offset is also the next row's start (and the caret is drawn
    // there), so stop just before it; a second End goes to the line end.
    const Row row = rowAt(rowOf(pos));
    if (row.lastInLine)
        return row.end;
    const size_t end = buffer_.prevCodepoint(row.end);
    return pos == end ? buffer_.lineEnd(row.line) : end;
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

void EditorView::ensureLayout()
{
    if (!style_.wrap) {
        if (wrap_.enabled())
            wrap_.disable();
        layoutDirty_ = false;
        return;
    }
    const float width = columnWidth();
    if (layoutDirty_ || !wrap_.enabled() || wrap_.width() != std::max(width, 1.0f)
        || layoutVersion_ != buffer_.version()) {
        wrap_.rebuild(*text_, width);
        layoutVersion_ = buffer_.version();
        layoutDirty_ = false;
    }
}

size_t EditorView::rowOf(size_t pos)
{
    const size_t line = buffer_.lineOf(pos);
    if (!wrap_.enabled() || wrap_.rowsIn(line) == 1)
        return wrap_.firstRow(line);
    wrap_.rowStarts(buffer_.line(line, scratch_), rowStarts_);
    const size_t local = pos - buffer_.lineStart(line);
    const auto it = std::upper_bound(rowStarts_.begin(), rowStarts_.end(), local);
    return wrap_.firstRow(line) + static_cast<size_t>(it - rowStarts_.begin()) - 1;
}

EditorView::Row EditorView::rowAt(size_t row)
{
    const size_t line = wrap_.lineAtRow(row);
    const size_t start = buffer_.lineStart(line);
    const size_t end = buffer_.lineEnd(line);
    if (!wrap_.enabled() || wrap_.rowsIn(line) == 1)
        return {line, start, end, true};

    wrap_.rowStarts(buffer_.line(line, scratch_), rowStarts_);
    const size_t r = std::min(row - wrap_.firstRow(line), rowStarts_.size() - 1);
    const bool last = r + 1 == rowStarts_.size();
    return {line, start + rowStarts_[r], last ? end : start + rowStarts_[r + 1], last};
}

std::string_view EditorView::rowText(const Row& row)
{
    const std::string_view line = buffer_.line(row.line, scratch_);
    return line.substr(row.start - buffer_.lineStart(row.line), row.end - row.start);
}

size_t EditorView::offsetInRow(const Row& row, float x)
{
    const size_t pos = row.start + text_->hitTest(rowText(row), x);
    // Past the end of a wrapped row: stay on this row (see rowEnd()).
    if (pos == row.end && !row.lastInLine && row.end > row.start)
        return buffer_.prevCodepoint(pos);
    return pos;
}

// ---------------------------------------------------------------------------
// Layout helpers
// ---------------------------------------------------------------------------

float EditorView::lineHeight() const
{
    return static_cast<float>(text_->font().lineHeight());
}

float EditorView::padding() const
{
    return std::round(lineHeight() * 0.5f);
}

float EditorView::gutterWidth() const
{
    if (!style_.lineNumbers)
        return 0.0f;
    size_t digits = 1;
    for (size_t n = buffer_.lineCount(); n >= 10; n /= 10)
        ++digits;
    digits = std::max<size_t>(digits, 3);
    return std::round(static_cast<float>(digits) * text_->advance('0') + 2.0f * padding());
}

SDL_FRect EditorView::textArea() const
{
    const float gutter = gutterWidth();
    return {bounds_.x + gutter, bounds_.y, std::max(0.0f, bounds_.w - gutter), bounds_.h};
}

float EditorView::columnWidth() const
{
    const float available = std::max(0.0f, textArea().w - 2.0f * padding());
    if (style_.maxColumnEms <= 0.0f)
        return available;
    return std::min(available, std::round(style_.maxColumnEms * text_->font().pixelSize()));
}

float EditorView::columnLeft() const
{
    const SDL_FRect area = textArea();
    return std::round(area.x + (area.w - columnWidth()) * 0.5f);
}

long long EditorView::visibleRows() const
{
    return std::max(1LL, static_cast<long long>(bounds_.h / lineHeight()));
}

size_t EditorView::offsetAt(float x, float y)
{
    const float docY = y - bounds_.y + std::round(scrollY_);
    const long long last = static_cast<long long>(wrap_.rowCount()) - 1;
    const long long row = std::clamp(static_cast<long long>(std::floor(docY / lineHeight())), 0LL, last);
    return offsetInRow(rowAt(static_cast<size_t>(row)), x - columnLeft() + std::round(scrollX_));
}

float EditorView::caretX()
{
    const Row row = rowAt(rowOf(cursor_));
    return text_->measure(rowText(row), cursor_ - row.start);
}

void EditorView::revealCaret()
{
    ensureLayout();
    const float lh = lineHeight();
    const float y = static_cast<float>(rowOf(cursor_)) * lh;
    if (y < scrollY_)
        scrollY_ = y;
    else if (y + lh > scrollY_ + bounds_.h)
        scrollY_ = y + lh - bounds_.h;

    if (!style_.wrap) {
        const float x = caretX();
        const float visibleWidth = columnWidth();
        const float margin = std::min(4.0f * text_->advance(' '), visibleWidth * 0.25f);
        if (x < scrollX_ + margin)
            scrollX_ = x - margin;
        else if (x > scrollX_ + visibleWidth - margin)
            scrollX_ = x - visibleWidth + margin;
    }

    clampScroll();
}

void EditorView::clampScroll()
{
    const float contentHeight = static_cast<float>(wrap_.rowCount()) * lineHeight();
    scrollY_ = std::clamp(scrollY_, 0.0f, std::max(0.0f, contentHeight - bounds_.h));
    scrollX_ = style_.wrap ? 0.0f : std::max(0.0f, scrollX_);
}

void EditorView::reset()
{
    cursor_ = anchor_ = 0;
    desiredX_ = -1.0f;
    scrollX_ = scrollY_ = 0.0f;
    dragging_ = false;
    layoutDirty_ = true;
    history_.clear();
    highlighter_.reset();
    resetBlink();
}

bool EditorView::isOverText(float x, float y) const
{
    return ui::contains(textArea(), x, y);
}

size_t EditorView::cursorLine() const
{
    return buffer_.lineOf(cursor_);
}

size_t EditorView::cursorColumn() const
{
    size_t column = 0;
    for (size_t p = buffer_.lineStart(cursorLine()); p < cursor_; ++p) {
        if (!utf8::isContinuation(buffer_.at(p)))
            ++column;
    }
    return column;
}

size_t EditorView::topLine()
{
    ensureLayout();
    return wrap_.lineAtRow(static_cast<size_t>(std::max(0.0f, scrollY_) / lineHeight()));
}

// ---------------------------------------------------------------------------
// Caret blink
// ---------------------------------------------------------------------------

void EditorView::setFocused(bool focused)
{
    focused_ = focused;
    resetBlink();
}

void EditorView::resetBlink()
{
    blinkStart_ = SDL_GetTicks();
}

bool EditorView::caretVisible() const
{
    if (!focused_)
        return false;
    return blinkMs_ == 0 || ((SDL_GetTicks() - blinkStart_) / blinkMs_) % 2 == 0;
}

Sint32 EditorView::blinkTimeoutMs() const
{
    if (!focused_ || blinkMs_ == 0)
        return -1;
    const Uint64 elapsed = SDL_GetTicks() - blinkStart_;
    return static_cast<Sint32>(blinkMs_ - elapsed % blinkMs_) + 1;
}


namespace {

SDL_FColor tokenColor(syntax::Token token)
{
    switch (token) {
    case syntax::Token::Keyword: return theme::kSynKeyword;
    case syntax::Token::Type: return theme::kSynType;
    case syntax::Token::Literal: return theme::kSynLiteral;
    case syntax::Token::String: return theme::kSynString;
    case syntax::Token::Number: return theme::kSynNumber;
    case syntax::Token::Comment: return theme::kSynComment;
    case syntax::Token::Preprocessor: return theme::kSynPreprocessor;
    case syntax::Token::Function: return theme::kSynFunction;
    case syntax::Token::Text: break;
    }
    return theme::kText;
}

} // namespace

// Converts the line's token spans into color runs for the row [from, to).
void EditorView::colorRuns(size_t from, size_t to)
{
    colorRuns_.clear();
    for (const syntax::Span& span : spans_) {
        if (span.end <= from || span.start >= to)
            continue;
        colorRuns_.push_back({std::max<size_t>(span.start, from) - from, tokenColor(span.token)});
        colorRuns_.push_back({std::min<size_t>(span.end, to) - from, theme::kText});
    }
}

void EditorView::setLanguage(const syntax::Language* language)
{
    highlighter_.setLanguage(language);
}

void EditorView::goToLine(size_t line)
{
    ensureLayout();
    line = std::min(line, buffer_.lineCount() - 1);
    moveTo(buffer_.lineStart(line), false);
    // Put the line near the top, with a little context above it.
    scrollY_ = static_cast<float>(wrap_.firstRow(line)) * lineHeight() - 2.0f * lineHeight();
    clampScroll();
    resetBlink();
}
// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void EditorView::draw(SDL_Renderer* renderer)
{
    ensureLayout();
    clampScroll();

    const float lh = lineHeight();
    const float sx = std::round(scrollX_);
    const float sy = std::round(scrollY_);
    const SDL_FRect area = textArea();
    const float originX = columnLeft() - sx;

    const size_t rowCount = wrap_.rowCount();
    const auto firstRow = static_cast<size_t>(sy / lh);
    const size_t lastRow = std::min(rowCount, static_cast<size_t>((sy + bounds_.h) / lh) + 1); // exclusive
    const size_t caretLine = cursorLine();
    const size_t selStart = selectionStart();
    const size_t selEnd = selectionEnd();
    const auto rowY = [&](size_t row) { return bounds_.y + static_cast<float>(row) * lh - sy; };

    ui::fillRect(renderer, bounds_, theme::kEditorBg);

    // Text area: line highlight + selection rects first, then all text in one batch.
    const SDL_Rect textClip = ui::toPixelRect(area);
    SDL_SetRenderClipRect(renderer, &textClip);

    size_t row = firstRow;
    size_t line = wrap_.lineAtRow(firstRow);
    size_t localRow = firstRow - wrap_.firstRow(line);
    while (row < lastRow) {
        const size_t lineStart = buffer_.lineStart(line);
        const std::string_view text = buffer_.line(line, scratch_);
        wrap_.rowStarts(text, rowStarts_);
        if (highlighter_.language())
            highlighter_.highlight(line, text, spans_);

        for (; localRow < rowStarts_.size() && row < lastRow; ++localRow, ++row) {
            const float y = rowY(row);
            const bool lastInLine = localRow + 1 == rowStarts_.size();
            const size_t from = rowStarts_[localRow];
            const size_t to = lastInLine ? text.size() : rowStarts_[localRow + 1];
            const std::string_view piece = text.substr(from, to - from);
            const size_t start = lineStart + from;
            const size_t end = lineStart + to;

            if (style_.highlightLine && line == caretLine && !hasSelection())
                ui::fillRect(renderer, {area.x, y, area.w, lh}, theme::kCurrentLine);

            if (selStart < selEnd && selStart <= end && selEnd > start) {
                const float x0 = text_->measure(piece, std::max(selStart, start) - start);
                float x1 = text_->measure(piece, std::min(selEnd, end) - start);
                if (lastInLine && selEnd > end) // selection continues past the line break
                    x1 += text_->advance(' ');
                ui::fillRect(renderer, {originX + x0, y, x1 - x0, lh}, theme::kSelection);
            }

            if (highlighter_.language()) {
                colorRuns(from, to);
                text_->draw(originX, y, piece, theme::kText, colorRuns_.data(), colorRuns_.size(), area.x,
                            area.x + area.w);
            } else {
                text_->draw(originX, y, piece, theme::kText, area.x, area.x + area.w);
            }
        }
        ++line;
        localRow = 0;
    }
    text_->flush();

    if (caretVisible()) {
        const float width = std::max(1.0f, std::round(lh / 12.0f));
        ui::fillRect(renderer, {originX + std::round(caretX()), rowY(rowOf(cursor_)), width, lh}, theme::kCaret);
    }

    // Gutter with right-aligned line numbers, on each line's first row.
    if (style_.lineNumbers) {
        const SDL_FRect gutter{bounds_.x, bounds_.y, gutterWidth(), bounds_.h};
        const SDL_Rect gutterClip = ui::toPixelRect(gutter);
        SDL_SetRenderClipRect(renderer, &gutterClip);
        ui::fillRect(renderer, gutter, theme::kGutterBg);

        const float digitWidth = text_->advance('0');
        char number[24];
        const size_t lastLine = rowCount == 0 ? 0 : wrap_.lineAtRow(lastRow - 1);
        for (size_t l = wrap_.lineAtRow(firstRow); l <= lastLine && l < buffer_.lineCount(); ++l) {
            const size_t r = wrap_.firstRow(l);
            if (r < firstRow)
                continue; // first row scrolled off; its number is out of view
            const char* numberEnd = std::to_chars(number, number + sizeof(number), l + 1).ptr;
            const auto length = static_cast<size_t>(numberEnd - number);
            const float x = gutter.x + gutter.w - padding() - static_cast<float>(length) * digitWidth;
            text_->draw(x, rowY(r), {number, length},
                        l == caretLine ? theme::kGutterActive : theme::kGutterText);
        }
        text_->flush();
    }

    SDL_SetRenderClipRect(renderer, nullptr);
}
