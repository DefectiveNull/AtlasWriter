#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class TextBuffer;

// Basic, table-driven syntax highlighting: keywords, types, literals,
// strings, numbers, comments, preprocessor lines / annotations and function
// calls. Not a parser: it's a per-line scanner whose only memory across lines
// is a one-byte state (inside a block comment or a multi-line string).
namespace syntax {

enum class Token : uint8_t { Text, Keyword, Type, Literal, String, Number, Comment, Preprocessor, Function };

struct Span
{
    uint32_t start; // byte offsets within the line
    uint32_t end;
    Token token;
};

struct Language; // defined in Syntax.cpp

// The language for a FileType name ("C++", "Python", ...), or null when
// there is none (plain text, or a language without a table yet).
const Language* languageFor(std::string_view fileTypeName);

using State = uint8_t; // what the line starts inside of; 0 = nothing

// Scans one line (without its '\n') starting in `state`, appending spans to
// `out` if given, and returns the state the next line starts in.
State scanLine(const Language& language, std::string_view line, State state, std::vector<Span>* out);

} // namespace syntax

// Per-document cache of line start states, kept in step with edits.
//
// Edits only invalidate states from the edited line on; they are recomputed
// lazily, and only up to the lines actually being drawn.
class Highlighter
{
public:
    explicit Highlighter(const TextBuffer& buffer);

    void setLanguage(const syntax::Language* language);
    const syntax::Language* language() const { return language_; }

    void reset(); // the whole buffer changed
    // Lines [first, first + oldCount) were replaced by [first, first + newCount).
    void linesChanged(size_t first, size_t oldCount, size_t newCount);

    // Spans for `line`, whose text is `text`.
    void highlight(size_t line, std::string_view text, std::vector<syntax::Span>& out);

private:
    syntax::State stateAt(size_t line);

    const TextBuffer& buffer_;
    const syntax::Language* language_ = nullptr;
    std::vector<syntax::State> states_; // start state per line
    size_t valid_ = 1;                  // states_[0, valid_) are up to date
    std::string scratch_;
};
