#include "TerminalScreen.h"

#include <algorithm>
#include <cstdio>

TerminalScreen::TerminalScreen(int cols, int rows)
    : cols_(std::max(1, cols))
    , rows_(std::max(1, rows))
{
    reset();
}

TerminalScreen::Cell TerminalScreen::blank() const
{
    // Erased cells take the current background (xterm's behavior).
    Cell cell;
    cell.bg = pen_.bg;
    return cell;
}

void TerminalScreen::reset()
{
    main_.assign(static_cast<size_t>(rows_), Line(static_cast<size_t>(cols_)));
    alt_.assign(static_cast<size_t>(rows_), Line(static_cast<size_t>(cols_)));
    altActive_ = false;
    cursorX_ = cursorY_ = 0;
    wrapPending_ = false;
    pen_ = Cell{};
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
    autowrap_ = cursorVisible_ = true;
    appCursorKeys_ = bracketedPaste_ = false;
}

void TerminalScreen::resize(int cols, int rows)
{
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    if (cols == cols_ && rows == rows_)
        return;

    for (std::vector<Line>* g : {&main_, &alt_}) {
        const bool isMain = g == &main_;
        // Shrinking: drop lines from the top (into scrollback) so the cursor stays visible.
        if (rows < rows_) {
            // Only the grid the cursor is on needs it kept in view.
            const int cursorRow = (isMain == !altActive_) ? cursorY_ : 0;
            const int fromTop = std::max(0, cursorRow - (rows - 1));
            for (int i = 0; i < fromTop; ++i) {
                if (isMain)
                    scrollback_.push_back(std::move((*g)[static_cast<size_t>(i)]));
            }
            g->erase(g->begin(), g->begin() + fromTop);
            g->resize(static_cast<size_t>(rows));
        } else {
            g->resize(static_cast<size_t>(rows), Line(static_cast<size_t>(cols)));
        }
        for (Line& line : *g)
            line.resize(static_cast<size_t>(cols));
    }
    while (scrollback_.size() > kMaxScrollback)
        scrollback_.pop_front();

    cols_ = cols;
    rows_ = rows;
    cursorX_ = std::clamp(cursorX_, 0, cols_ - 1);
    cursorY_ = std::clamp(cursorY_, 0, rows_ - 1);
    savedX_ = std::clamp(savedX_, 0, cols_ - 1);
    savedY_ = std::clamp(savedY_, 0, rows_ - 1);
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
    wrapPending_ = false;
}

std::string TerminalScreen::takeResponses()
{
    std::string out;
    out.swap(responses_);
    return out;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

void TerminalScreen::feed(std::string_view bytes)
{
    for (const char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        switch (state_) {
        case State::Ground:
            if (utf8Remaining_ > 0) {
                if ((c & 0xC0) == 0x80) {
                    utf8Codepoint_ = (utf8Codepoint_ << 6) | (c & 0x3F);
                    if (--utf8Remaining_ == 0)
                        print(utf8Codepoint_);
                    continue;
                }
                utf8Remaining_ = 0;
                print(0xFFFD); // truncated sequence; reprocess this byte below
            }
            if (c == 0x1B) {
                state_ = State::Escape;
            } else if (c < 0x20 || c == 0x7F) {
                control(c);
            } else if (c < 0x80) {
                print(c);
            } else if ((c & 0xE0) == 0xC0) {
                utf8Codepoint_ = c & 0x1F;
                utf8Remaining_ = 1;
            } else if ((c & 0xF0) == 0xE0) {
                utf8Codepoint_ = c & 0x0F;
                utf8Remaining_ = 2;
            } else if ((c & 0xF8) == 0xF0) {
                utf8Codepoint_ = c & 0x07;
                utf8Remaining_ = 3;
            } else {
                print(0xFFFD);
            }
            break;

        case State::Escape:
            escape(c);
            break;

        case State::Charset: // ESC ( B and friends: character sets are ignored
            state_ = State::Ground;
            break;

        case State::Csi:
            if (c >= '0' && c <= '9') {
                if (params_.empty())
                    params_.push_back(0);
                int& p = params_.back();
                p = std::min((p < 0 ? 0 : p) * 10 + (c - '0'), 99999); // -1 = still empty
                paramStarted_ = true;
            } else if (c == ';' || c == ':') {
                if (params_.empty())
                    params_.push_back(-1); // leading empty parameter
                params_.push_back(-1);
                paramStarted_ = false;
            } else if (c >= 0x3C && c <= 0x3F) { // < = > ?
                privateMarker_ = static_cast<char>(c);
            } else if (c >= 0x20 && c <= 0x2F) {
                intermediate_ = static_cast<char>(c);
            } else if (c >= 0x40 && c <= 0x7E) {
                csi(c); // empty parameters stay -1, read as "default" by param()
                state_ = State::Ground;
            } else if (c == 0x1B) {
                state_ = State::Escape;
            } else if (c < 0x20) {
                control(c); // C0 controls execute even inside a sequence
            }
            break;

        case State::Osc:
            if (c == 0x07) {
                osc();
                state_ = State::Ground;
            } else if (c == 0x1B) {
                state_ = State::OscEscape;
            } else if (osc_.size() < 4096) {
                osc_ += static_cast<char>(c);
            }
            break;

        case State::OscEscape: // ESC \ ends the string
            osc();
            state_ = State::Ground;
            if (c != '\\')
                escape(c);
            break;

        case State::Ignore: // DCS, APC, PM, SOS: skipped to the string terminator
            if (c == 0x07)
                state_ = State::Ground;
            else if (c == 0x1B)
                state_ = State::IgnoreEscape;
            break;

        case State::IgnoreEscape:
            state_ = c == '\\' ? State::Ground : State::Ignore;
            break;
        }
    }
}

void TerminalScreen::escape(unsigned char c)
{
    state_ = State::Ground;
    switch (c) {
    case '[':
        state_ = State::Csi;
        params_.clear();
        paramStarted_ = false;
        privateMarker_ = 0;
        intermediate_ = 0;
        break;
    case ']':
        state_ = State::Osc;
        osc_.clear();
        break;
    case 'P':
    case 'X':
    case '^':
    case '_':
        state_ = State::Ignore;
        break;
    case '(':
    case ')':
    case '*':
    case '+':
        state_ = State::Charset;
        break;
    case '7':
        savedX_ = cursorX_;
        savedY_ = cursorY_;
        savedPen_ = pen_;
        break;
    case '8':
        moveTo(savedX_, savedY_);
        pen_ = savedPen_;
        break;
    case 'D':
        lineFeed();
        break;
    case 'E':
        cursorX_ = 0;
        lineFeed();
        break;
    case 'M':
        reverseIndex();
        break;
    case 'c':
        reset();
        break;
    default: // '=', '>' (keypad modes) and anything unknown
        break;
    }
}

void TerminalScreen::control(unsigned char c)
{
    switch (c) {
    case '\b':
        if (cursorX_ > 0)
            --cursorX_;
        wrapPending_ = false;
        break;
    case '\t':
        cursorX_ = std::min(cols_ - 1, (cursorX_ / 8 + 1) * 8);
        wrapPending_ = false;
        break;
    case '\n':
    case '\v':
    case '\f':
        lineFeed();
        break;
    case '\r':
        cursorX_ = 0;
        wrapPending_ = false;
        break;
    default: // BEL, SO, SI, ... are ignored
        break;
    }
}

int TerminalScreen::param(size_t i, int fallback) const
{
    return i < params_.size() && params_[i] >= 0 ? params_[i] : fallback;
}

void TerminalScreen::csi(unsigned char final)
{
    if (privateMarker_ == '?' && (final == 'h' || final == 'l')) {
        setMode(final == 'h');
        return;
    }
    if (privateMarker_ != 0 && final != 'c')
        return; // other private sequences (e.g. xterm key modifier options)
    if (intermediate_ != 0)
        return; // e.g. CSI n SP q (cursor shape)

    const int n = std::max(1, param(0, 1));
    Line& row = grid()[static_cast<size_t>(cursorY_)];
    switch (final) {
    case 'A':
        moveTo(cursorX_, std::max(cursorY_ >= scrollTop_ ? scrollTop_ : 0, cursorY_ - n));
        break;
    case 'B':
        moveTo(cursorX_, std::min(cursorY_ <= scrollBottom_ ? scrollBottom_ : rows_ - 1, cursorY_ + n));
        break;
    case 'C':
        moveTo(cursorX_ + n, cursorY_);
        break;
    case 'D':
        moveTo(cursorX_ - n, cursorY_);
        break;
    case 'E':
        moveTo(0, cursorY_ + n);
        break;
    case 'F':
        moveTo(0, cursorY_ - n);
        break;
    case 'G':
    case '`':
        moveTo(param(0, 1) - 1, cursorY_);
        break;
    case 'd':
        moveTo(cursorX_, param(0, 1) - 1);
        break;
    case 'H':
    case 'f':
        moveTo(param(1, 1) - 1, param(0, 1) - 1);
        break;
    case 'J': {
        const int mode = param(0, 0);
        if (mode == 0) {
            eraseCells(cursorY_, cursorX_, cols_);
            for (int y = cursorY_ + 1; y < rows_; ++y)
                eraseCells(y, 0, cols_);
        } else if (mode == 1) {
            for (int y = 0; y < cursorY_; ++y)
                eraseCells(y, 0, cols_);
            eraseCells(cursorY_, 0, cursorX_ + 1);
        } else if (mode == 2) {
            for (int y = 0; y < rows_; ++y)
                eraseCells(y, 0, cols_);
        } else if (mode == 3 && !altActive_) {
            scrollback_.clear();
        }
        break;
    }
    case 'K': {
        const int mode = param(0, 0);
        if (mode == 0)
            eraseCells(cursorY_, cursorX_, cols_);
        else if (mode == 1)
            eraseCells(cursorY_, 0, cursorX_ + 1);
        else
            eraseCells(cursorY_, 0, cols_);
        break;
    }
    case 'X':
        eraseCells(cursorY_, cursorX_, std::min(cols_, cursorX_ + n));
        break;
    case '@': {
        const int count = std::min(n, cols_ - cursorX_);
        row.insert(row.begin() + cursorX_, static_cast<size_t>(count), blank());
        row.resize(static_cast<size_t>(cols_));
        break;
    }
    case 'P': {
        const int count = std::min(n, cols_ - cursorX_);
        row.erase(row.begin() + cursorX_, row.begin() + cursorX_ + count);
        row.resize(static_cast<size_t>(cols_), blank());
        break;
    }
    case 'L':
        if (cursorY_ >= scrollTop_ && cursorY_ <= scrollBottom_)
            scrollDown(cursorY_, scrollBottom_, n);
        break;
    case 'M':
        if (cursorY_ >= scrollTop_ && cursorY_ <= scrollBottom_)
            scrollUp(cursorY_, scrollBottom_, n);
        break;
    case 'S':
        scrollUp(scrollTop_, scrollBottom_, n);
        break;
    case 'T':
        scrollDown(scrollTop_, scrollBottom_, n);
        break;
    case 'm':
        sgr();
        break;
    case 'r': {
        const int top = param(0, 1) - 1;
        const int bottom = param(1, rows_) - 1;
        if (top < bottom && bottom < rows_) {
            scrollTop_ = std::max(0, top);
            scrollBottom_ = bottom;
        } else {
            scrollTop_ = 0;
            scrollBottom_ = rows_ - 1;
        }
        moveTo(0, 0);
        break;
    }
    case 's':
        savedX_ = cursorX_;
        savedY_ = cursorY_;
        break;
    case 'u':
        moveTo(savedX_, savedY_);
        break;
    case 'n':
        if (param(0, 0) == 6) {
            char reply[32];
            std::snprintf(reply, sizeof(reply), "\x1b[%d;%dR", cursorY_ + 1, cursorX_ + 1);
            responses_ += reply;
        } else if (param(0, 0) == 5) {
            responses_ += "\x1b[0n";
        }
        break;
    case 'c':
        if (privateMarker_ == 0)
            responses_ += "\x1b[?1;2c"; // "VT100 with advanced video"
        break;
    default: // window ops ('t'), modes without '?' ('h'/'l'), ...
        break;
    }
}

void TerminalScreen::setMode(bool on)
{
    for (size_t i = 0; i < std::max<size_t>(1, params_.size()); ++i) {
        switch (param(i, 0)) {
        case 1:
            appCursorKeys_ = on;
            break;
        case 7:
            autowrap_ = on;
            break;
        case 25:
            cursorVisible_ = on;
            break;
        case 47:
        case 1047:
        case 1049:
            if (on == altActive_)
                break;
            if (on) {
                savedX_ = cursorX_;
                savedY_ = cursorY_;
                altActive_ = true;
                for (int y = 0; y < rows_; ++y)
                    eraseCells(y, 0, cols_);
            } else {
                altActive_ = false;
                moveTo(savedX_, savedY_);
            }
            scrollTop_ = 0;
            scrollBottom_ = rows_ - 1;
            break;
        case 2004:
            bracketedPaste_ = on;
            break;
        default: // mouse reporting, focus events, win32-input-mode, ...
            break;
        }
    }
}

void TerminalScreen::sgr()
{
    if (params_.empty()) {
        pen_ = Cell{};
        return;
    }
    for (size_t i = 0; i < params_.size(); ++i) {
        const int p = param(i, 0);
        if (p == 0) {
            pen_ = Cell{};
        } else if (p == 1) {
            pen_.attr |= kBold;
        } else if (p == 2) {
            pen_.attr |= kFaint;
        } else if (p == 3) {
            pen_.attr |= kItalic;
        } else if (p == 4) {
            pen_.attr |= kUnderline;
        } else if (p == 7) {
            pen_.attr |= kInverse;
        } else if (p == 8) {
            pen_.attr |= kHidden;
        } else if (p == 9) {
            pen_.attr |= kStrike;
        } else if (p == 21 || p == 22) {
            pen_.attr &= static_cast<uint8_t>(~(kBold | kFaint));
        } else if (p == 23) {
            pen_.attr &= static_cast<uint8_t>(~kItalic);
        } else if (p == 24) {
            pen_.attr &= static_cast<uint8_t>(~kUnderline);
        } else if (p == 27) {
            pen_.attr &= static_cast<uint8_t>(~kInverse);
        } else if (p == 28) {
            pen_.attr &= static_cast<uint8_t>(~kHidden);
        } else if (p == 29) {
            pen_.attr &= static_cast<uint8_t>(~kStrike);
        } else if (p >= 30 && p <= 37) {
            pen_.fg = kIndexed | static_cast<uint32_t>(p - 30);
        } else if (p >= 40 && p <= 47) {
            pen_.bg = kIndexed | static_cast<uint32_t>(p - 40);
        } else if (p >= 90 && p <= 97) {
            pen_.fg = kIndexed | static_cast<uint32_t>(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            pen_.bg = kIndexed | static_cast<uint32_t>(p - 100 + 8);
        } else if (p == 39) {
            pen_.fg = kDefault;
        } else if (p == 49) {
            pen_.bg = kDefault;
        } else if (p == 38 || p == 48) {
            uint32_t color = kDefault;
            if (param(i + 1, 0) == 5) {
                color = kIndexed | static_cast<uint32_t>(std::clamp(param(i + 2, 0), 0, 255));
                i += 2;
            } else if (param(i + 1, 0) == 2) {
                const auto channel = [&](size_t k) { return static_cast<uint32_t>(std::clamp(param(k, 0), 0, 255)); };
                color = (channel(i + 2) << 16) | (channel(i + 3) << 8) | channel(i + 4);
                i += 4;
            }
            (p == 38 ? pen_.fg : pen_.bg) = color;
        }
    }
}

void TerminalScreen::osc()
{
    // "0;title" or "2;title" set the window title; others are ignored.
    if (osc_.size() >= 2 && (osc_[0] == '0' || osc_[0] == '2') && osc_[1] == ';')
        title_ = osc_.substr(2);
    osc_.clear();
}

// ---------------------------------------------------------------------------
// Grid operations
// ---------------------------------------------------------------------------

void TerminalScreen::print(uint32_t codepoint)
{
    if (wrapPending_) {
        wrapPending_ = false;
        if (autowrap_) {
            cursorX_ = 0;
            lineFeed();
        }
    }
    Cell cell = pen_;
    cell.codepoint = codepoint;
    grid()[static_cast<size_t>(cursorY_)][static_cast<size_t>(cursorX_)] = cell;
    if (cursorX_ == cols_ - 1)
        wrapPending_ = true;
    else
        ++cursorX_;
}

void TerminalScreen::moveTo(int x, int y)
{
    cursorX_ = std::clamp(x, 0, cols_ - 1);
    cursorY_ = std::clamp(y, 0, rows_ - 1);
    wrapPending_ = false;
}

void TerminalScreen::lineFeed()
{
    wrapPending_ = false;
    if (cursorY_ == scrollBottom_)
        scrollUp(scrollTop_, scrollBottom_, 1);
    else if (cursorY_ < rows_ - 1)
        ++cursorY_;
}

void TerminalScreen::reverseIndex()
{
    wrapPending_ = false;
    if (cursorY_ == scrollTop_)
        scrollDown(scrollTop_, scrollBottom_, 1);
    else if (cursorY_ > 0)
        --cursorY_;
}

void TerminalScreen::scrollUp(int top, int bottom, int n)
{
    std::vector<Line>& g = grid();
    n = std::min(n, bottom - top + 1);
    for (int i = 0; i < n; ++i) {
        // Lines leaving the top of the full main screen go to scrollback.
        if (top == 0 && !altActive_) {
            scrollback_.push_back(std::move(g[0]));
            if (scrollback_.size() > kMaxScrollback)
                scrollback_.pop_front();
        }
        g.erase(g.begin() + top);
        g.insert(g.begin() + bottom, Line(static_cast<size_t>(cols_), blank()));
    }
}

void TerminalScreen::scrollDown(int top, int bottom, int n)
{
    std::vector<Line>& g = grid();
    n = std::min(n, bottom - top + 1);
    for (int i = 0; i < n; ++i) {
        g.erase(g.begin() + bottom);
        g.insert(g.begin() + top, Line(static_cast<size_t>(cols_), blank()));
    }
}

void TerminalScreen::eraseCells(int row, int from, int to)
{
    Line& line = grid()[static_cast<size_t>(row)];
    from = std::clamp(from, 0, cols_);
    to = std::clamp(to, 0, cols_);
    std::fill(line.begin() + from, line.begin() + to, blank());
}
