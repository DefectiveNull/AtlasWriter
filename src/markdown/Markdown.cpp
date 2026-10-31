#include "Markdown.h"

#include "markdown/Html.h"

#include <algorithm>
#include <array>

namespace md {

namespace {

bool isSpace(char c)
{
    return c == ' ' || c == '\t';
}

bool isAlnum(char c)
{
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isPunct(char c)
{
    const auto u = static_cast<unsigned char>(c);
    return u > 0x20 && u < 0x7F && !isAlnum(c);
}

std::string_view trimLeft(std::string_view s)
{
    size_t i = 0;
    while (i < s.size() && isSpace(s[i]))
        ++i;
    return s.substr(i);
}

std::string_view trimRight(std::string_view s)
{
    size_t n = s.size();
    while (n > 0 && isSpace(s[n - 1]))
        --n;
    return s.substr(0, n);
}

// Leading whitespace in columns (tabs to multiples of 4), and the bytes it spans.
int indentOf(std::string_view s, size_t& bytes)
{
    int columns = 0;
    bytes = 0;
    while (bytes < s.size() && isSpace(s[bytes])) {
        columns = s[bytes] == '\t' ? (columns / 4 + 1) * 4 : columns + 1;
        ++bytes;
    }
    return columns;
}

// "---", "***", "___", optionally with spaces between.
bool isRule(std::string_view s)
{
    char ch = 0;
    int count = 0;
    for (const char c : s) {
        if (isSpace(c))
            continue;
        if (c != '-' && c != '*' && c != '_')
            return false;
        if (ch != 0 && c != ch)
            return false;
        ch = c;
        ++count;
    }
    return count >= 3;
}

int headingLevel(std::string_view s, std::string_view& content)
{
    int level = 0;
    while (level < static_cast<int>(s.size()) && s[static_cast<size_t>(level)] == '#')
        ++level;
    if (level == 0 || level > 6)
        return 0;
    const std::string_view rest = s.substr(static_cast<size_t>(level));
    if (!rest.empty() && !isSpace(rest[0]))
        return 0; // "#hashtag" is not a heading

    content = trimRight(trimLeft(rest));
    // Optional closing hashes: "## Title ##"
    size_t end = content.size();
    while (end > 0 && content[end - 1] == '#')
        --end;
    if (end == 0 || isSpace(content[end - 1]))
        content = trimRight(content.substr(0, end));
    return level;
}

bool isFence(std::string_view s, char& ch, size_t& length)
{
    if (s.size() < 3 || (s[0] != '`' && s[0] != '~'))
        return false;
    size_t n = 0;
    while (n < s.size() && s[n] == s[0])
        ++n;
    if (n < 3)
        return false;
    ch = s[0];
    length = n;
    return true;
}

bool isClosingFence(std::string_view s, char ch, size_t length)
{
    s = trimRight(trimLeft(s));
    return s.size() >= length && std::all_of(s.begin(), s.end(), [&](char c) { return c == ch; });
}

// "- item", "* item", "+ item", "1. item", "1) item"
bool listMarker(std::string_view s, std::string& marker, std::string_view& content)
{
    if (s.empty())
        return false;
    size_t n = 0;
    if (s[0] == '-' || s[0] == '*' || s[0] == '+') {
        n = 1;
        marker.clear();
    } else {
        while (n < s.size() && n < 9 && s[n] >= '0' && s[n] <= '9')
            ++n;
        if (n == 0 || n >= s.size() || (s[n] != '.' && s[n] != ')'))
            return false;
        marker.assign(s.substr(0, n));
        marker += '.';
        ++n;
    }
    if (n < s.size() && !isSpace(s[n]))
        return false;
    content = trimLeft(s.substr(n));
    return true;
}

// Strips "> " markers and returns the quote depth.
int stripQuotes(std::string_view& s)
{
    int depth = 0;
    while (true) {
        size_t bytes = 0;
        const int indent = indentOf(s, bytes);
        if (indent > 3 || bytes >= s.size() || s[bytes] != '>')
            return depth;
        s.remove_prefix(bytes + 1);
        if (!s.empty() && s[0] == ' ')
            s.remove_prefix(1);
        ++depth;
    }
}

// Splits a table row on unescaped '|', dropping the optional outer pipes.
std::vector<std::string_view> splitCells(std::string_view row)
{
    row = trimRight(trimLeft(row));
    if (!row.empty() && row.front() == '|')
        row.remove_prefix(1);
    if (!row.empty() && row.back() == '|' && (row.size() < 2 || row[row.size() - 2] != '\\'))
        row.remove_suffix(1);

    std::vector<std::string_view> cells;
    size_t start = 0;
    for (size_t i = 0; i <= row.size(); ++i) {
        if (i == row.size() || (row[i] == '|' && (i == 0 || row[i - 1] != '\\'))) {
            cells.push_back(trimRight(trimLeft(row.substr(start, i - start))));
            start = i + 1;
        }
    }
    return cells;
}

// "| :--- | :---: | ---: |" -> column alignments. Needs at least one pipe
// so a plain "---" stays a horizontal rule.
bool parseDelimiterRow(std::string_view line, std::vector<Align>& align)
{
    if (line.find('|') == std::string_view::npos)
        return false;
    align.clear();
    for (std::string_view cell : splitCells(line)) {
        if (cell.empty())
            return false;
        const bool left = cell.front() == ':';
        const bool right = cell.back() == ':';
        if (left)
            cell.remove_prefix(1);
        if (right && !cell.empty())
            cell.remove_suffix(1);
        if (cell.empty() || !std::all_of(cell.begin(), cell.end(), [](char c) { return c == '-'; }))
            return false;
        align.push_back(left && right ? Align::Center : right ? Align::Right : left ? Align::Left : Align::Default);
    }
    return !align.empty();
}

TableRow tableRow(std::string_view line, size_t columns, bool header)
{
    TableRow row;
    row.header = header;
    const std::vector<std::string_view> cells = splitCells(line);
    for (size_t i = 0; i < columns; ++i) // extra cells are ignored, missing ones empty
        row.cells.push_back(i < cells.size() ? parseInline(cells[i]) : std::vector<Span>{});
    return row;
}

} // namespace

std::vector<Block> parse(std::string_view text)
{
    std::vector<Block> blocks;
    Block current;
    std::string raw;          // inline source of the open paragraph / list item
    size_t lastLineStart = 0; // where the paragraph's last line begins in `raw`
    bool open = false;        // `current` is an open paragraph, list item, table or code block
    bool hardBreak = false;   // previous line ended with a hard line break
    bool inCode = false;
    char fenceChar = 0;
    size_t fenceLength = 0;

    HtmlBlockStart htmlKind = HtmlBlockStart::None; // != None while inside an HTML block
    std::string htmlEnd;
    std::string html;
    size_t htmlLine = 0;
    int htmlQuote = 0;

    const auto close = [&] {
        if (!open)
            return;
        if (current.type == BlockType::Paragraph || current.type == BlockType::ListItem)
            current.spans = parseInline(raw);
        blocks.push_back(std::move(current));
        current = Block{};
        raw.clear();
        open = false;
        hardBreak = false;
    };
    const auto finishHtml = [&] {
        parseHtml(html, htmlLine, htmlQuote, blocks);
        html.clear();
        htmlKind = HtmlBlockStart::None;
    };
    const auto containsIgnoreCase = [](std::string_view haystack, std::string_view needle) {
        return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
                   return (a >= 'A' && a <= 'Z' ? a - 'A' + 'a' : a) == b;
               }) != haystack.end();
    };

    size_t lineNo = 0;
    for (size_t pos = 0; pos <= text.size(); ++lineNo) {
        const size_t newline = std::min(text.find('\n', pos), text.size());
        std::string_view line = text.substr(pos, newline - pos);
        pos = newline + 1;

        if (inCode) {
            std::string_view inner = line;
            for (int q = 0; q < current.quote; ++q)
                stripQuotes(inner);
            if (isClosingFence(inner, fenceChar, fenceLength)) {
                inCode = false;
                close();
                continue;
            }
            if (!current.code.empty() || lineNo > current.sourceLine + 1)
                current.code += '\n';
            current.code.append(inner);
            continue;
        }

        const int quote = stripQuotes(line);
        const std::string_view body = trimLeft(line);

        if (htmlKind != HtmlBlockStart::None) {
            const bool untilBlank = htmlKind == HtmlBlockStart::UntilBlank || htmlKind == HtmlBlockStart::LoneTag;
            if (untilBlank && body.empty()) {
                finishHtml();
                continue;
            }
            html.append(line);
            html += '\n';
            if (!untilBlank && containsIgnoreCase(line, htmlEnd))
                finishHtml();
            continue;
        }

        if (body.empty()) {
            close();
            continue;
        }

        // Rows continue a table until a blank line or another kind of block.
        if (open && current.type == BlockType::Table) {
            char ch = 0;
            size_t len = 0;
            std::string_view ignored;
            std::string marker;
            const bool otherBlock = quote != current.quote || isFence(body, ch, len) || headingLevel(body, ignored) > 0
                                 || htmlBlockStart(line, marker) != HtmlBlockStart::None;
            if (!otherBlock) {
                current.rows.push_back(tableRow(body, current.align.size(), false));
                continue;
            }
            close();
        }

        size_t indentBytes = 0;
        const int indent = indentOf(line, indentBytes);
        std::string_view content;
        std::string marker;
        std::vector<Align> align;
        const bool paragraphOpen = open && current.type == BlockType::Paragraph && quote == current.quote;

        if (isFence(body, fenceChar, fenceLength)) {
            close();
            current.type = BlockType::Code;
            current.quote = quote;
            current.sourceLine = lineNo;
            open = inCode = true;
        } else if (const HtmlBlockStart kind = htmlBlockStart(line, htmlEnd);
                   kind != HtmlBlockStart::None && !(kind == HtmlBlockStart::LoneTag && open)) {
            close();
            htmlKind = kind;
            htmlLine = lineNo;
            htmlQuote = quote;
            html.assign(line);
            html += '\n';
            if ((kind == HtmlBlockStart::UntilTag || kind == HtmlBlockStart::UntilComment)
                && containsIgnoreCase(line.substr(1), htmlEnd))
                finishHtml(); // opened and closed on one line
        } else if (paragraphOpen && parseDelimiterRow(body, align)
                   && splitCells(std::string_view(raw).substr(lastLineStart)).size() == align.size()) {
            // The paragraph's last line was the header row.
            const std::string header = raw.substr(lastLineStart);
            const size_t headerLine = lineNo - 1;
            if (lastLineStart > 0) {
                raw.resize(lastLineStart - 1); // drop the separator too
                close();
            } else {
                current = Block{};
                raw.clear();
                open = false;
            }
            current.type = BlockType::Table;
            current.quote = quote;
            current.sourceLine = headerLine;
            current.align = std::move(align);
            current.rows.push_back(tableRow(header, current.align.size(), true));
            open = true;
            continue;
        } else if (isRule(body)) {
            close();
            blocks.push_back({BlockType::Rule, 0, quote, -1, {}, lineNo, {}, {}, {}, {}});
        } else if (const int level = headingLevel(body, content); level > 0) {
            close();
            blocks.push_back({BlockType::Heading, level, quote, -1, {}, lineNo, parseInline(content), {}, {}, {}});
        } else if (listMarker(body, marker, content)) {
            close();
            current.type = BlockType::ListItem;
            current.level = std::min(indent / 2, 8);
            current.quote = quote;
            current.marker = std::move(marker);
            current.sourceLine = lineNo;
            if (content.size() >= 3 && content[0] == '[' && content[2] == ']'
                && (content.size() == 3 || isSpace(content[3]))) {
                if (content[1] == ' ')
                    current.task = 0;
                else if (content[1] == 'x' || content[1] == 'X')
                    current.task = 1;
                if (current.task >= 0)
                    content = trimLeft(content.substr(3));
            }
            raw.assign(trimRight(content));
            lastLineStart = 0;
            open = true;
        } else if (open && current.type != BlockType::Code && quote <= current.quote) {
            // Continuation (also "lazy" lines that dropped their '>' markers).
            raw += hardBreak ? '\n' : ' ';
            lastLineStart = raw.size();
            raw.append(trimRight(body));
        } else {
            close();
            current.type = BlockType::Paragraph;
            current.quote = quote;
            current.sourceLine = lineNo;
            raw.assign(trimRight(body));
            lastLineStart = 0;
            open = true;
        }

        // Two trailing spaces or a trailing backslash force a line break.
        if (open && !inCode && current.type != BlockType::Table) {
            hardBreak = line.ends_with("  ");
            if (!raw.empty() && raw.back() == '\\') {
                raw.pop_back();
                hardBreak = true;
            }
        }
    }
    if (htmlKind != HtmlBlockStart::None)
        finishHtml();
    close();
    return blocks;
}

std::vector<Span> parseInline(std::string_view s)
{
    std::vector<Span> spans;
    std::string text;
    uint8_t mdStyle = 0;              // emphasis toggled by Markdown delimiters
    std::array<int, 8> htmlCounts{};  // open inline HTML tags per style bit

    const auto style = [&] {
        uint8_t st = mdStyle;
        for (size_t b = 0; b < htmlCounts.size(); ++b) {
            if (htmlCounts[b] > 0)
                st |= static_cast<uint8_t>(1u << b);
        }
        return st;
    };
    const auto flush = [&] {
        if (!text.empty())
            spans.push_back({std::move(text), style()});
        text.clear();
    };
    const auto hasLater = [&](std::string_view delimiter, size_t from) {
        return s.find(delimiter, from) != std::string_view::npos;
    };

    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];

        if (c == '\\' && i + 1 < s.size() && isPunct(s[i + 1])) {
            text += s[i + 1];
            i += 2;
            continue;
        }

        if (c == '&') {
            if (const size_t n = decodeEntity(s, i, text); n > 0) {
                i += n;
                continue;
            }
        }

        if (c == '`') {
            size_t n = 0;
            while (i + n < s.size() && s[i + n] == '`')
                ++n;
            const std::string_view ticks = s.substr(i, n);
            const size_t close = s.find(ticks, i + n);
            if (close == std::string_view::npos) {
                text.append(ticks);
                i += n;
                continue;
            }
            std::string_view code = s.substr(i + n, close - i - n);
            if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ')
                code = code.substr(1, code.size() - 2);
            flush();
            spans.push_back({std::string(code), static_cast<uint8_t>(style() | kCode)});
            i = close + n;
            continue;
        }

        if (c == '*' || c == '_' || (c == '~' && i + 1 < s.size() && s[i + 1] == '~')) {
            size_t n = 0;
            while (i + n < s.size() && s[i + n] == c)
                ++n;
            const char before = i > 0 ? s[i - 1] : ' ';
            const char after = i + n < s.size() ? s[i + n] : ' ';
            bool canOpen = !isSpace(after);
            bool canClose = !isSpace(before);
            if (c == '_') { // snake_case is not emphasis
                canOpen = canOpen && !isAlnum(before);
                canClose = canClose && !isAlnum(after);
            }

            const auto toggle = [&](uint8_t flag, size_t length) {
                if (n < length)
                    return;
                const std::string_view delimiter = s.substr(i, length);
                if ((mdStyle & flag) ? canClose : (canOpen && hasLater(delimiter, i + n))) {
                    flush();
                    mdStyle ^= flag;
                    n -= length;
                }
            };
            const size_t total = n;
            if (c == '~') {
                toggle(kStrike, 2);
            } else {
                toggle(kBold, 2);
                toggle(kItalic, 1);
            }
            text.append(n, c); // delimiters that matched nothing are literal
            i += total;
            continue;
        }

        if (c == '[' || (c == '!' && i + 1 < s.size() && s[i + 1] == '[')) {
            const bool image = c == '!';
            const size_t open = i + (image ? 2 : 1);
            const size_t closeBracket = s.find(']', open);
            if (closeBracket != std::string_view::npos && closeBracket + 1 < s.size() && s[closeBracket + 1] == '(') {
                const size_t closeParen = s.find(')', closeBracket + 2);
                if (closeParen != std::string_view::npos) {
                    flush();
                    Span span;
                    span.text.assign(s.substr(open, closeBracket - open));
                    if (image && span.text.empty())
                        span.text = "image";
                    span.style = static_cast<uint8_t>(style() | (image ? kImage : kLink));
                    // (dest "title") or (<dest with spaces> "title")
                    std::string_view dest = trimLeft(s.substr(closeBracket + 2, closeParen - closeBracket - 2));
                    if (dest.starts_with('<'))
                        dest = dest.substr(1, std::min(dest.find('>'), dest.size()) - 1);
                    else
                        dest = dest.substr(0, std::min(dest.find_first_of(" \t"), dest.size()));
                    span.url.assign(dest);
                    spans.push_back(std::move(span));
                    i = closeParen + 1;
                    continue;
                }
            }
        }

        if (c == '<') {
            const size_t close = s.find('>', i + 1);
            if (close != std::string_view::npos) {
                const std::string_view inner = s.substr(i + 1, close - i - 1);
                if (inner.starts_with("http://") || inner.starts_with("https://") || inner.starts_with("mailto:")) {
                    flush();
                    spans.push_back({std::string(inner), static_cast<uint8_t>(style() | kLink)});
                    i = close + 1;
                    continue;
                }
            }

            // Inline HTML: formatting tags style the text, <br> breaks the
            // line, <img> shows its alt text, everything else is dropped.
            Tag tag;
            if (const size_t n = parseTag(s, i, tag); n > 0) {
                if (const uint8_t bit = inlineTagStyle(tag.name); bit != 0) {
                    flush();
                    int b = 0;
                    while ((1u << b) != bit)
                        ++b;
                    int& count = htmlCounts[static_cast<size_t>(b)];
                    if (tag.closing)
                        count = std::max(0, count - 1);
                    else if (!tag.selfClosing)
                        ++count;
                } else if (tag.name == "br") {
                    text += '\n';
                } else if (tag.name == "img" && !tag.closing) {
                    flush();
                    spans.push_back(imageSpan(tag, style()));
                }
                i += n;
                continue;
            }
        }

        text += c;
        ++i;
    }
    flush();
    return spans;
}

} // namespace md
