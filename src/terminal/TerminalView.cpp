#include "TerminalView.h"

#include "text/Font.h"
#include "ui/Draw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

// Dark+ style ANSI colors.
constexpr uint32_t kAnsi[16] = {
    0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xE5E5E5,
    0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
};
constexpr SDL_FColor kForeground{0.80f, 0.80f, 0.82f, 1.0f};

SDL_FColor rgb(uint32_t v)
{
    return {static_cast<float>((v >> 16) & 0xFF) / 255.0f, static_cast<float>((v >> 8) & 0xFF) / 255.0f,
            static_cast<float>(v & 0xFF) / 255.0f, 1.0f};
}

uint32_t palette(uint32_t index)
{
    if (index < 16)
        return kAnsi[index];
    if (index < 232) { // 6x6x6 cube
        static constexpr uint32_t levels[6] = {0, 95, 135, 175, 215, 255};
        const uint32_t i = index - 16;
        return (levels[i / 36] << 16) | (levels[(i / 6) % 6] << 8) | levels[i % 6];
    }
    const uint32_t gray = 8 + (index - 232) * 10; // grayscale ramp
    return (gray << 16) | (gray << 8) | gray;
}

void appendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool sameColor(const SDL_FColor& a, const SDL_FColor& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

} // namespace

TerminalView::TerminalView(TextRenderer& text, Uint32 eventType)
    : text_(text)
    , pty_(eventType)
    , screen_(80, 24)
{
}

float TerminalView::headerHeight() const
{
    return std::round(static_cast<float>(text_.font().lineHeight()) * 1.6f);
}

bool TerminalView::contains(float x, float y) const
{
    return ui::contains(bounds_, x, y);
}

bool TerminalView::isOverHeader(float x, float y) const
{
    return contains(x, y) && y < bounds_.y + headerHeight();
}

SDL_FRect TerminalView::closeButton() const
{
    const float h = headerHeight();
    return {bounds_.x + bounds_.w - h, bounds_.y, h, h};
}

bool TerminalView::isOverCloseButton(float x, float y) const
{
    return ui::contains(closeButton(), x, y);
}

void TerminalView::layoutGrid()
{
    cellW_ = std::max(1.0f, text_.advance('M'));
    cellH_ = static_cast<float>(std::max(1, text_.font().lineHeight()));
    const float pad = std::round(cellH_ * 0.4f);
    const int cols = std::max(2, static_cast<int>((bounds_.w - 2.0f * pad) / cellW_));
    const int rows = std::max(1, static_cast<int>((bounds_.h - headerHeight() - pad) / cellH_));
    if (cols != screen_.cols() || rows != screen_.rows()) {
        screen_.resize(cols, rows);
        pty_.resize(cols, rows);
    }
}

void TerminalView::ensureStarted(const std::string& directory)
{
    if (pty_.running())
        return;
    directory_ = directory;
    layoutGrid();
    exitedShown_ = false;
    if (!pty_.start(shell_, directory_, screen_.cols(), screen_.rows()))
        screen_.feed("\x1b[31mCould not start a shell.\x1b[0m\r\n");
}

bool TerminalView::onOutput()
{
    const std::string data = pty_.takeOutput();
    bool changed = false;
    if (!data.empty()) {
        screen_.feed(data);
        const std::string replies = screen_.takeResponses();
        if (!replies.empty())
            pty_.write(replies);
        changed = true;
    }
    if (pty_.exited() && !exitedShown_) {
        exitedShown_ = true;
        screen_.feed("\r\n\x1b[0;2m[process exited - press Enter to restart]\x1b[0m");
        changed = true;
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void TerminalView::send(std::string_view bytes)
{
    viewOffset_ = 0; // typing jumps back to the live screen
    hasSelection_ = false;
    if (exitedShown_) {
        if (bytes == "\r") {
            pty_.stop();
            screen_.feed("\r\n");
            ensureStarted(directory_);
        }
        return;
    }
    pty_.write(bytes);
}

void TerminalView::paste()
{
    char* clipboard = SDL_GetClipboardText();
    if (!clipboard)
        return;
    std::string text;
    for (const char* p = clipboard; *p; ++p) { // newlines become Enter
        if (*p == '\r' && p[1] == '\n')
            continue;
        text += *p == '\n' ? '\r' : *p;
    }
    SDL_free(clipboard);
    if (screen_.bracketedPaste())
        text = "\x1b[200~" + text + "\x1b[201~";
    send(text);
}

const TerminalScreen::Line* TerminalView::lineAt(long long index) const
{
    const auto scrollback = static_cast<long long>(screen_.scrollbackSize());
    if (index < 0)
        return nullptr;
    if (index < scrollback)
        return &screen_.scrollbackLine(static_cast<size_t>(index));
    if (index - scrollback < screen_.rows())
        return &screen_.line(static_cast<int>(index - scrollback));
    return nullptr;
}

TerminalView::Point TerminalView::cellAt(float x, float y) const
{
    const float pad = std::round(cellH_ * 0.4f);
    const float gridTop = bounds_.y + headerHeight();
    const int row = std::clamp(static_cast<int>(std::floor((y - gridTop) / cellH_)), 0, screen_.rows() - 1);
    const int col = std::clamp(static_cast<int>(std::floor((x - bounds_.x - pad) / cellW_ + 0.5f)), 0, screen_.cols());
    const long long top = static_cast<long long>(screen_.scrollbackSize()) - static_cast<long long>(viewOffset_);
    return {top + row, col};
}

void TerminalView::copySelection()
{
    if (!hasSelection_)
        return;
    Point a = selStart_, b = selEnd_;
    if (b.line < a.line || (b.line == a.line && b.col < a.col))
        std::swap(a, b);
    std::string text;
    for (long long l = a.line; l <= b.line; ++l) {
        const TerminalScreen::Line* line = lineAt(l);
        if (!line)
            continue;
        const int from = l == a.line ? a.col : 0;
        const int to = l == b.line ? b.col : static_cast<int>(line->size());
        std::string row;
        for (int c = from; c < to && c < static_cast<int>(line->size()); ++c)
            appendUtf8(row, (*line)[static_cast<size_t>(c)].codepoint);
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
        text += row;
        if (l != b.line)
            text += '\n';
    }
    SDL_SetClipboardText(text.c_str());
}

bool TerminalView::handleKey(const SDL_KeyboardEvent& key)
{
    const bool ctrl = (key.mod & SDL_KMOD_CTRL) != 0;
    const bool shift = (key.mod & SDL_KMOD_SHIFT) != 0;
    const bool alt = (key.mod & SDL_KMOD_ALT) != 0;
    const SDL_Keycode k = key.key;

    if (ctrl && shift && !alt && (k == SDLK_C || k == SDLK_V)) {
        if (k == SDLK_C)
            copySelection();
        else
            paste();
        return true;
    }
    if (ctrl && !shift && !alt && k == SDLK_C && hasSelection_) { // copy instead of interrupting
        copySelection();
        hasSelection_ = false;
        return true;
    }
    if (shift && (k == SDLK_PAGEUP || k == SDLK_PAGEDOWN)) {
        const size_t page = static_cast<size_t>(std::max(1, screen_.rows() - 1));
        viewOffset_ = k == SDLK_PAGEUP ? std::min(screen_.scrollbackSize(), viewOffset_ + page)
                                       : (viewOffset_ > page ? viewOffset_ - page : 0);
        return true;
    }

    // xterm modifier parameter: 1 + shift + 2*alt + 4*ctrl
    const int m = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    char buf[32];
    const auto cursorKey = [&](char final) {
        if (m > 1)
            std::snprintf(buf, sizeof(buf), "\x1b[1;%d%c", m, final);
        else
            std::snprintf(buf, sizeof(buf), screen_.applicationCursorKeys() ? "\x1bO%c" : "\x1b[%c", final);
        send(buf);
        return true;
    };
    const auto tildeKey = [&](int code) {
        if (m > 1)
            std::snprintf(buf, sizeof(buf), "\x1b[%d;%d~", code, m);
        else
            std::snprintf(buf, sizeof(buf), "\x1b[%d~", code);
        send(buf);
        return true;
    };

    switch (k) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        send("\r");
        return true;
    case SDLK_BACKSPACE:
        send(ctrl ? "\x08" : (alt ? "\x1b\x7f" : "\x7f"));
        return true;
    case SDLK_TAB:
        send(shift ? "\x1b[Z" : "\t");
        return true;
    case SDLK_ESCAPE:
        send("\x1b");
        return true;
    case SDLK_UP: return cursorKey('A');
    case SDLK_DOWN: return cursorKey('B');
    case SDLK_RIGHT: return cursorKey('C');
    case SDLK_LEFT: return cursorKey('D');
    case SDLK_HOME: return cursorKey('H');
    case SDLK_END: return cursorKey('F');
    case SDLK_INSERT: return tildeKey(2);
    case SDLK_DELETE: return tildeKey(3);
    case SDLK_PAGEUP: return tildeKey(5);
    case SDLK_PAGEDOWN: return tildeKey(6);
    case SDLK_F1:
    case SDLK_F2:
    case SDLK_F3:
    case SDLK_F4: {
        const char final = static_cast<char>('P' + (k - SDLK_F1));
        if (m > 1)
            std::snprintf(buf, sizeof(buf), "\x1b[1;%d%c", m, final);
        else
            std::snprintf(buf, sizeof(buf), "\x1bO%c", final);
        send(buf);
        return true;
    }
    case SDLK_F5: return tildeKey(15);
    case SDLK_F6: return tildeKey(17);
    case SDLK_F7: return tildeKey(18);
    case SDLK_F8: return tildeKey(19);
    case SDLK_F9: return tildeKey(20);
    case SDLK_F10: return tildeKey(21);
    case SDLK_F11: return tildeKey(23);
    case SDLK_F12: return tildeKey(24);
    default:
        break;
    }

    // Ctrl+letter -> control character. Ctrl+Alt is AltGr on Windows: leave
    // that to text input.
    if (ctrl && !alt) {
        char c = 0;
        bool handled = true;
        if (k >= SDLK_A && k <= SDLK_Z)
            c = static_cast<char>(k - SDLK_A + 1);
        else if (k == SDLK_LEFTBRACKET)
            c = 0x1B;
        else if (k == SDLK_BACKSLASH)
            c = 0x1C;
        else if (k == SDLK_RIGHTBRACKET)
            c = 0x1D;
        else if (k == SDLK_SPACE || k == SDLK_2)
            c = 0x00;
        else if (k == SDLK_6)
            c = 0x1E;
        else if (k == SDLK_MINUS || k == SDLK_SLASH)
            c = 0x1F;
        else
            handled = false;
        if (handled) {
            send(std::string_view(&c, 1));
            return true;
        }
    }
    // Alt+key -> ESC prefix (no text input is generated for these).
    if (alt && !ctrl && ((k >= SDLK_A && k <= SDLK_Z) || (k >= SDLK_0 && k <= SDLK_9))) {
        char seq[2] = {'\x1b', static_cast<char>(shift && k >= SDLK_A ? k - 32 : k)};
        send(std::string_view(seq, 2));
        return true;
    }
    return false;
}

bool TerminalView::handleEvent(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.button != SDL_BUTTON_LEFT || !contains(event.button.x, event.button.y)
            || isOverHeader(event.button.x, event.button.y))
            return false;
        selStart_ = selEnd_ = cellAt(event.button.x, event.button.y);
        selecting_ = true;
        hasSelection_ = false;
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        if (!selecting_)
            return false;
        selEnd_ = cellAt(event.motion.x, event.motion.y);
        hasSelection_ = selEnd_.line != selStart_.line || selEnd_.col != selStart_.col;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        selecting_ = false;
        return false;
    case SDL_EVENT_MOUSE_WHEEL: {
        if (!contains(event.wheel.mouse_x, event.wheel.mouse_y))
            return false;
        const float dy = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
        const long long next = static_cast<long long>(viewOffset_) + static_cast<long long>(std::lround(dy * 3.0f));
        const size_t before = viewOffset_;
        viewOffset_ = static_cast<size_t>(std::clamp(next, 0LL, static_cast<long long>(screen_.scrollbackSize())));
        return viewOffset_ != before;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (!focused_)
            return false;
        send(event.text.text);
        return true;
    case SDL_EVENT_KEY_DOWN:
        return focused_ && handleKey(event.key);
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

SDL_FColor TerminalView::color(uint32_t value, bool foreground, bool bold) const
{
    if (value & TerminalScreen::kDefault)
        return foreground ? kForeground : theme::kTerminalBg;
    if (value & TerminalScreen::kIndexed) {
        uint32_t index = value & 0xFF;
        if (foreground && bold && index < 8)
            index += 8; // bold as bright, like most terminals
        return rgb(palette(index));
    }
    return rgb(value);
}

void TerminalView::draw(SDL_Renderer* renderer)
{
    layoutGrid();
    const float header = headerHeight();
    const float lh = static_cast<float>(text_.font().lineHeight());
    const float pad = std::round(cellH_ * 0.4f);

    ui::fillRect(renderer, bounds_, theme::kTerminalBg);

    // Header: title, close button, separator line (also the resize handle).
    ui::fillRect(renderer, {bounds_.x, bounds_.y, bounds_.w, header}, theme::kPanelBg);
    ui::fillRect(renderer, {bounds_.x, bounds_.y, bounds_.w, 1.0f}, focused_ ? theme::kMenuActive : theme::kBorder);
    std::string title = "TERMINAL";
    if (!pty_.shellName().empty())
        title += "  \xC2\xB7  " + pty_.shellName();
    const float textY = bounds_.y + std::round((header - lh) * 0.5f);
    text_.draw(bounds_.x + pad, textY, title, theme::kMuted);
    const SDL_FRect close = closeButton();
    text_.draw(std::round(close.x + (close.w - text_.advance(0xD7)) * 0.5f), textY, "\xC3\x97", theme::kMuted);
    text_.flush();

    const float gridTop = bounds_.y + header;
    const float left = bounds_.x + pad;
    const SDL_Rect clip = ui::toPixelRect({bounds_.x, gridTop, bounds_.w, bounds_.h - header});
    SDL_SetRenderClipRect(renderer, &clip);

    Point selA = selStart_, selB = selEnd_;
    if (selB.line < selA.line || (selB.line == selA.line && selB.col < selA.col))
        std::swap(selA, selB);

    const long long top = static_cast<long long>(screen_.scrollbackSize()) - static_cast<long long>(viewOffset_);
    const auto cellX = [&](int col) { return std::floor(left + static_cast<float>(col) * cellW_); };
    for (int r = 0; r < screen_.rows(); ++r) {
        const long long index = top + r;
        const TerminalScreen::Line* line = lineAt(index);
        if (!line)
            continue;
        const float y = gridTop + static_cast<float>(r) * cellH_;
        rowText_.clear();
        runs_.clear();
        SDL_FColor runColor = kForeground;

        const int cols = std::min(static_cast<int>(line->size()), screen_.cols());
        int bgStart = 0;
        SDL_FColor bgColor = theme::kTerminalBg;
        for (int c = 0; c <= cols; ++c) {
            // Background runs (flushed when the color changes).
            SDL_FColor bg = theme::kTerminalBg;
            SDL_FColor fg = kForeground;
            const TerminalScreen::Cell* cell = c < cols ? &(*line)[static_cast<size_t>(c)] : nullptr;
            if (cell) {
                const bool bold = (cell->attr & TerminalScreen::kBold) != 0;
                fg = color(cell->fg, true, bold);
                bg = color(cell->bg, false, false);
                if (cell->attr & TerminalScreen::kInverse)
                    std::swap(fg, bg);
                if (cell->attr & TerminalScreen::kFaint)
                    fg.a = 0.6f;
                if (cell->attr & TerminalScreen::kHidden)
                    fg = bg;
            }
            if (!cell || !sameColor(bg, bgColor)) {
                if (!sameColor(bgColor, theme::kTerminalBg))
                    ui::fillRect(renderer, {cellX(bgStart), y, cellX(c) - cellX(bgStart), cellH_}, bgColor);
                bgStart = c;
                bgColor = bg;
            }
            if (!cell)
                break;

            if (cell->attr & (TerminalScreen::kUnderline | TerminalScreen::kStrike)) {
                const float thickness = std::max(1.0f, std::round(lh / 16.0f));
                const float lineY = (cell->attr & TerminalScreen::kUnderline)
                                        ? y + static_cast<float>(text_.font().ascent()) + thickness
                                        : y + std::round(cellH_ * 0.55f);
                ui::fillRect(renderer, {cellX(c), lineY, cellX(c + 1) - cellX(c), thickness}, fg);
            }

            if (!sameColor(fg, runColor) || runs_.empty()) {
                runs_.push_back({rowText_.size(), fg});
                runColor = fg;
            }
            const uint32_t cp = cell->codepoint;
            appendUtf8(rowText_, cp < 0x20 ? ' ' : cp);
        }

        // Selection highlight over the cells.
        if (hasSelection_ && index >= selA.line && index <= selB.line) {
            const int from = index == selA.line ? selA.col : 0;
            const int to = index == selB.line ? selB.col : screen_.cols();
            if (to > from)
                ui::fillRect(renderer, {cellX(from), y, cellX(to) - cellX(from), cellH_}, theme::kSelection);
        }
        text_.draw(left, y, rowText_, kForeground, runs_.data(), runs_.size());
    }

    // Cursor: a block when focused, an outline when not; hidden while
    // looking at scrollback.
    if (viewOffset_ == 0 && screen_.cursorVisible() && !exitedShown_) {
        const SDL_FRect cursor{cellX(screen_.cursorX()), gridTop + static_cast<float>(screen_.cursorY()) * cellH_,
                               std::max(1.0f, cellX(screen_.cursorX() + 1) - cellX(screen_.cursorX())), cellH_};
        if (focused_)
            ui::fillRect(renderer, cursor, {theme::kCaret.r, theme::kCaret.g, theme::kCaret.b, 0.55f});
        else
            ui::strokeRect(renderer, cursor, theme::kMuted);
    }
    text_.flush();
    SDL_SetRenderClipRect(renderer, nullptr);
}
