#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

// The terminal's model: a grid of cells plus scrollback, driven by the VT /
// xterm escape sequences shells and ConPTY emit.
//
// Covers what interactive shells and common TUIs need: cursor movement,
// erase/insert/delete, scroll regions, SGR colors (16, 256 and truecolor),
// the alternate screen, cursor visibility, application cursor keys,
// bracketed paste, window titles, and cursor position / device attribute
// reports. Every codepoint takes one cell (no double-width CJK yet).
class TerminalScreen
{
public:
    // Colors: kDefault, an indexed palette entry (kIndexed | 0..255), or 24-bit RGB.
    static constexpr uint32_t kDefault = 1u << 25;
    static constexpr uint32_t kIndexed = 1u << 24;

    enum Attr : uint8_t {
        kBold = 1 << 0,
        kFaint = 1 << 1,
        kItalic = 1 << 2,
        kUnderline = 1 << 3,
        kInverse = 1 << 4,
        kStrike = 1 << 5,
        kHidden = 1 << 6,
    };

    struct Cell
    {
        uint32_t codepoint = ' ';
        uint32_t fg = kDefault;
        uint32_t bg = kDefault;
        uint8_t attr = 0;
    };
    using Line = std::vector<Cell>;

    static constexpr size_t kMaxScrollback = 5000;

    TerminalScreen(int cols, int rows);

    void resize(int cols, int rows);
    void feed(std::string_view bytes);
    // Replies the program asked for (e.g. cursor position); send to the pty.
    std::string takeResponses();

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    const Line& line(int row) const { return grid()[static_cast<size_t>(row)]; }
    size_t scrollbackSize() const { return altActive_ ? 0 : scrollback_.size(); }
    const Line& scrollbackLine(size_t i) const { return scrollback_[i]; }

    int cursorX() const { return cursorX_; }
    int cursorY() const { return cursorY_; }
    bool cursorVisible() const { return cursorVisible_; }
    bool applicationCursorKeys() const { return appCursorKeys_; }
    bool bracketedPaste() const { return bracketedPaste_; }
    const std::string& title() const { return title_; }

private:
    enum class State { Ground, Escape, Charset, Csi, Osc, OscEscape, Ignore, IgnoreEscape };

    std::vector<Line>& grid() { return altActive_ ? alt_ : main_; }
    const std::vector<Line>& grid() const { return altActive_ ? alt_ : main_; }
    Cell blank() const;

    void print(uint32_t codepoint);
    void control(unsigned char c);
    void escape(unsigned char c);
    void csi(unsigned char final);
    void sgr();
    void osc();
    void setMode(bool on);

    int param(size_t i, int fallback) const;
    void moveTo(int x, int y);
    void lineFeed();
    void reverseIndex();
    void scrollUp(int top, int bottom, int n);
    void scrollDown(int top, int bottom, int n);
    void eraseCells(int row, int from, int to); // [from, to)
    void reset();

    int cols_, rows_;
    std::vector<Line> main_, alt_;
    std::deque<Line> scrollback_;
    bool altActive_ = false;

    int cursorX_ = 0, cursorY_ = 0;
    bool wrapPending_ = false; // at the right margin; the next character wraps
    int savedX_ = 0, savedY_ = 0;
    Cell pen_;      // attributes for new characters (codepoint unused)
    Cell savedPen_;
    int scrollTop_ = 0, scrollBottom_ = 0; // inclusive
    bool autowrap_ = true;
    bool cursorVisible_ = true;
    bool appCursorKeys_ = false;
    bool bracketedPaste_ = false;
    std::string title_;
    std::string responses_;

    // Parser
    State state_ = State::Ground;
    std::vector<int> params_;
    bool paramStarted_ = false;
    char privateMarker_ = 0; // '?', '>', ...
    char intermediate_ = 0;
    std::string osc_;
    uint32_t utf8Codepoint_ = 0;
    int utf8Remaining_ = 0;
};
