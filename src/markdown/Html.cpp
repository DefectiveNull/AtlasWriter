#include "Html.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>

namespace md {

namespace {

char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool isWhite(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool isAlpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

bool startsWithIgnoreCase(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && equalsIgnoreCase(s.substr(0, prefix.size()), prefix);
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && isWhite(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && isWhite(s.back()))
        s.remove_suffix(1);
    return s;
}

void appendUtf8(std::string& out, uint32_t cp)
{
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;
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

struct NamedEntity
{
    const char* name;
    uint32_t codepoint;
};

// The entities that actually show up in Markdown documents.
constexpr NamedEntity kEntities[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
    {"nbsp", 0xA0}, {"ensp", 0x2002}, {"emsp", 0x2003}, {"thinsp", 0x2009},
    {"zwj", 0x200D}, {"zwnj", 0x200C}, {"shy", 0xAD},
    {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"sect", 0xA7}, {"para", 0xB6},
    {"hellip", 0x2026}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"minus", 0x2212},
    {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
    {"laquo", 0xAB}, {"raquo", 0xBB}, {"bull", 0x2022}, {"middot", 0xB7},
    {"times", 0xD7}, {"divide", 0xF7}, {"plusmn", 0xB1}, {"deg", 0xB0}, {"micro", 0xB5},
    {"frac12", 0xBD}, {"frac14", 0xBC}, {"frac34", 0xBE}, {"sup1", 0xB9}, {"sup2", 0xB2}, {"sup3", 0xB3},
    {"larr", 0x2190}, {"uarr", 0x2191}, {"rarr", 0x2192}, {"darr", 0x2193}, {"harr", 0x2194},
    {"lArr", 0x21D0}, {"rArr", 0x21D2}, {"hArr", 0x21D4},
    {"le", 0x2264}, {"ge", 0x2265}, {"ne", 0x2260}, {"asymp", 0x2248}, {"infin", 0x221E},
    {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5}, {"cent", 0xA2},
    {"iexcl", 0xA1}, {"iquest", 0xBF}, {"check", 0x2713}, {"cross", 0x2717},
    {"hearts", 0x2665}, {"star", 0x2606}, {"starf", 0x2605},
    {"Auml", 0xC4}, {"Ouml", 0xD6}, {"Uuml", 0xDC}, {"auml", 0xE4}, {"ouml", 0xF6}, {"uuml", 0xFC},
    {"szlig", 0xDF}, {"eacute", 0xE9}, {"egrave", 0xE8}, {"aacute", 0xE1}, {"agrave", 0xE0},
    {"ccedil", 0xE7}, {"ntilde", 0xF1}, {"oacute", 0xF3}, {"iacute", 0xED}, {"uacute", 0xFA},
    {"atilde", 0xE3}, {"otilde", 0xF5}, {"acirc", 0xE2}, {"ecirc", 0xEA}, {"ocirc", 0xF4},
};

// CommonMark's HTML block type 6 tag names: these start a block that runs
// to the next blank line.
constexpr const char* kBlockTags[] = {
    "address", "article", "aside", "base", "basefont", "blockquote", "body", "caption", "center",
    "col", "colgroup", "dd", "details", "dialog", "dir", "div", "dl", "dt", "fieldset", "figcaption",
    "figure", "footer", "form", "frame", "frameset", "h1", "h2", "h3", "h4", "h5", "h6", "head",
    "header", "hr", "html", "iframe", "legend", "li", "link", "main", "menu", "menuitem", "nav",
    "noframes", "ol", "optgroup", "option", "p", "param", "search", "section", "summary", "table",
    "tbody", "td", "tfoot", "th", "thead", "title", "tr", "track", "ul",
};

// Content of these is never shown.
constexpr const char* kSkippedTags[] = {
    "script", "style", "head", "title", "template", "noscript", "svg", "iframe", "object",
    "video", "audio", "canvas", "select", "textarea", "math",
};

// Tags that separate paragraphs.
constexpr const char* kParagraphTags[] = {
    "p", "div", "section", "article", "header", "footer", "main", "nav", "aside", "figure",
    "figcaption", "details", "center", "address", "dl", "dd", "form", "fieldset", "legend",
    "body", "html", "picture", "search", "dialog", "menu",
};

template <size_t N>
bool oneOf(std::string_view name, const char* const (&list)[N])
{
    return std::any_of(std::begin(list), std::end(list), [&](const char* s) { return name == s; });
}

int headingLevel(std::string_view name)
{
    return name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6' ? name[1] - '0' : 0;
}

int bitIndex(uint8_t bit)
{
    int i = 0;
    while (bit > 1) {
        bit >>= 1;
        ++i;
    }
    return i;
}

Align parseAlign(std::string_view attributes)
{
    std::string value = attribute(attributes, "align");
    if (value.empty()) {
        // style="text-align: center"
        const std::string style = attribute(attributes, "style");
        const size_t at = style.find("text-align");
        if (at != std::string::npos) {
            size_t i = style.find(':', at);
            if (i != std::string::npos) {
                ++i;
                while (i < style.size() && isWhite(style[i]))
                    ++i;
                size_t end = i;
                while (end < style.size() && isAlpha(style[end]))
                    ++end;
                value = style.substr(i, end - i);
            }
        }
    }
    for (char& c : value)
        c = lower(c);
    if (value == "left")
        return Align::Left;
    if (value == "center")
        return Align::Center;
    if (value == "right")
        return Align::Right;
    return Align::Default;
}

// Builds blocks from a stream of tags and text.
class Converter
{
public:
    Converter(std::vector<Block>& out, size_t sourceLine, int quote)
        : out_(out)
        , sourceLine_(sourceLine)
        , baseQuote_(quote)
    {
    }

    void run(std::string_view html)
    {
        std::string chunk;
        size_t i = 0;
        while (i < html.size()) {
            if (html[i] == '<') {
                Tag tag;
                if (const size_t n = parseTag(html, i, tag); n > 0) {
                    flushChunk(chunk);
                    if (!tag.name.empty())
                        handleTag(tag);
                    i += n;
                    continue;
                }
            } else if (html[i] == '&') {
                if (const size_t n = decodeEntity(html, i, chunk); n > 0) {
                    i += n;
                    continue;
                }
            }
            chunk += html[i++];
        }
        flushChunk(chunk);
        if (inPre_)
            finishPre();
        if (inTable_)
            finishTable();
        endBlock();
    }

private:
    uint8_t style() const
    {
        uint8_t s = 0;
        for (size_t i = 0; i < counts_.size(); ++i) {
            if (counts_[i] > 0)
                s |= static_cast<uint8_t>(1u << i);
        }
        return s;
    }

    bool inCell() const { return inTable_ && !inCaption_; }

    // Where inline content currently goes; opens a paragraph if needed.
    // Null inside a table but outside any cell (that text is dropped).
    std::vector<Span>* target()
    {
        if (inCell()) {
            if (table_.rows.empty() || table_.rows.back().cells.empty())
                return nullptr;
            return &table_.rows.back().cells.back();
        }
        if (!open_)
            startBlock(BlockType::Paragraph);
        return &current_.spans;
    }

    bool hasContent()
    {
        if (!text_.empty())
            return true;
        if (inCell())
            return !table_.rows.empty() && !table_.rows.back().cells.empty() && !table_.rows.back().cells.back().empty();
        return open_ && !current_.spans.empty();
    }

    void flushText()
    {
        if (text_.empty())
            return;
        if (std::vector<Span>* spans = target())
            spans->push_back({std::move(text_), textStyle_});
        text_.clear();
    }

    void put(char c) { put(c, style()); }

    void put(char c, uint8_t st)
    {
        if (!text_.empty() && st != textStyle_)
            flushText();
        if (text_.empty())
            textStyle_ = st;
        text_ += c;
    }

    void addText(std::string_view s)
    {
        for (const char c : s) {
            if (isWhite(c)) {
                if (!pendingSpace_)
                    spaceStyle_ = style(); // "with <b>bold" keeps the space unstyled
                pendingSpace_ = true;
                continue;
            }
            if (inCell() && target() == nullptr)
                continue;
            if (pendingSpace_ && hasContent())
                put(' ', spaceStyle_);
            pendingSpace_ = false;
            if (!inCell() && !open_)
                startBlock(BlockType::Paragraph);
            put(c);
        }
    }

    void addBreak()
    {
        if (inCell() && target() == nullptr)
            return;
        if (!inCell() && !open_)
            return; // a <br> before any text adds nothing
        pendingSpace_ = false;
        put('\n');
    }

    void addSpan(Span span)
    {
        if (inCell() && target() == nullptr)
            return;
        if (pendingSpace_ && hasContent())
            put(' ', spaceStyle_);
        pendingSpace_ = false;
        flushText();
        if (std::vector<Span>* spans = target())
            spans->push_back(std::move(span));
    }

    void flushChunk(std::string& chunk)
    {
        if (chunk.empty())
            return;
        if (skipDepth_ > 0) {
            // dropped
        } else if (inPre_) {
            pre_ += chunk;
        } else {
            addText(chunk);
        }
        chunk.clear();
    }

    void startBlock(BlockType type)
    {
        endBlock();
        current_ = Block{};
        current_.type = type;
        current_.quote = baseQuote_ + quote_;
        current_.sourceLine = sourceLine_;
        open_ = true;
    }

    void endBlock()
    {
        if (inCell())
            return;
        flushText();
        pendingSpace_ = false;
        if (!open_)
            return;
        trimTrailing(current_.spans);
        if (!current_.spans.empty())
            out_.push_back(std::move(current_));
        current_ = Block{};
        open_ = false;
    }

    static void trimTrailing(std::vector<Span>& spans)
    {
        while (!spans.empty()) {
            std::string& text = spans.back().text;
            while (!text.empty() && (text.back() == ' ' || text.back() == '\n'))
                text.pop_back();
            if (!text.empty())
                return;
            spans.pop_back();
        }
    }

    void finishPre()
    {
        inPre_ = false;
        std::string_view code = pre_;
        if (!code.empty() && code.front() == '\n')
            code.remove_prefix(1);
        while (!code.empty() && (code.back() == '\n' || code.back() == '\r'))
            code.remove_suffix(1);
        Block block;
        block.type = BlockType::Code;
        block.quote = baseQuote_ + quote_;
        block.sourceLine = sourceLine_;
        block.code.assign(code);
        out_.push_back(std::move(block));
    }

    void endCell()
    {
        flushText();
        pendingSpace_ = false;
        if (!table_.rows.empty() && !table_.rows.back().cells.empty())
            trimTrailing(table_.rows.back().cells.back());
    }

    void endRow()
    {
        endCell();
        if (table_.rows.empty())
            return;
        TableRow& row = table_.rows.back();
        if (row.cells.empty()) {
            table_.rows.pop_back();
            return;
        }
        row.header = inHead_ || (rowHeaderCells_ == row.cells.size());
    }

    void finishTable()
    {
        endRow();
        inTable_ = false;
        inCaption_ = false;
        if (!table_.rows.empty())
            out_.push_back(std::move(table_));
        table_ = Block{};
    }

    void handleTag(const Tag& tag)
    {
        const std::string& name = tag.name;

        if (skipDepth_ > 0) {
            if (name == skipTag_)
                skipDepth_ += tag.closing ? -1 : (tag.selfClosing ? 0 : 1);
            return;
        }
        if (inPre_) {
            if (tag.closing && name == "pre")
                finishPre();
            else if (name == "br")
                pre_ += '\n';
            return;
        }
        if (!tag.closing && !tag.selfClosing && oneOf(name, kSkippedTags)) {
            skipTag_ = name;
            skipDepth_ = 1;
            return;
        }

        if (const uint8_t bit = inlineTagStyle(name); bit != 0) {
            int& count = counts_[static_cast<size_t>(bitIndex(bit))];
            if (tag.closing)
                count = std::max(0, count - 1);
            else if (!tag.selfClosing)
                ++count;
            return;
        }

        if (name == "br") {
            addBreak();
            return;
        }
        if (name == "img") {
            if (!tag.closing)
                addSpan(imageSpan(tag, style()));
            return;
        }
        if (name == "input") {
            const std::string type = attribute(tag.attributes, "type");
            if (equalsIgnoreCase(type, "checkbox") && open_ && current_.type == BlockType::ListItem
                && current_.spans.empty() && text_.empty()) {
                bool checked = false;
                attribute(tag.attributes, "checked", &checked);
                current_.task = checked ? 1 : 0;
            }
            return;
        }

        // Tables
        if (name == "table") {
            if (!tag.closing && !inTable_) {
                endBlock();
                inTable_ = true;
                table_ = Block{};
                table_.type = BlockType::Table;
                table_.quote = baseQuote_ + quote_;
                table_.sourceLine = sourceLine_;
            } else if (tag.closing && inTable_) {
                finishTable();
            }
            return;
        }
        if (inTable_) {
            if (name == "caption") {
                // Shown as a paragraph above the table (the table block is
                // only pushed at </table>).
                endCell();
                inCaption_ = !tag.closing;
                if (inCaption_)
                    startBlock(BlockType::Paragraph);
                else
                    endBlock();
                return;
            }
            if (name == "thead") {
                inHead_ = !tag.closing;
                return;
            }
            if (name == "tr") {
                endRow();
                if (!tag.closing) {
                    table_.rows.emplace_back();
                    rowHeaderCells_ = 0;
                }
                return;
            }
            if (name == "td" || name == "th") {
                endCell();
                if (tag.closing)
                    return;
                if (table_.rows.empty()) {
                    table_.rows.emplace_back();
                    rowHeaderCells_ = 0;
                }
                TableRow& row = table_.rows.back();
                row.cells.emplace_back();
                if (name == "th")
                    ++rowHeaderCells_;
                const size_t column = row.cells.size() - 1;
                const Align align = parseAlign(tag.attributes);
                if (table_.align.size() <= column)
                    table_.align.resize(column + 1, Align::Default);
                if (align != Align::Default && table_.align[column] == Align::Default)
                    table_.align[column] = align;
                return;
            }
            if (inCell()) {
                // Block structure inside a cell collapses to line breaks.
                if (oneOf(name, kParagraphTags) || name == "li" || name == "hr" || headingLevel(name) > 0) {
                    if (hasContent())
                        addBreak();
                }
                return;
            }
        }

        if (const int level = headingLevel(name); level > 0) {
            endBlock();
            if (!tag.closing) {
                startBlock(BlockType::Heading);
                current_.level = level;
            }
            return;
        }
        if (name == "hr") {
            endBlock();
            Block rule;
            rule.type = BlockType::Rule;
            rule.quote = baseQuote_ + quote_;
            rule.sourceLine = sourceLine_;
            out_.push_back(std::move(rule));
            return;
        }
        if (name == "pre") {
            endBlock();
            if (!tag.closing) {
                inPre_ = true;
                pre_.clear();
            }
            return;
        }
        if (name == "blockquote") {
            endBlock();
            quote_ = tag.closing ? std::max(0, quote_ - 1) : quote_ + 1;
            return;
        }
        if (name == "ul" || name == "ol") {
            endBlock();
            if (tag.closing) {
                if (!lists_.empty())
                    lists_.pop_back();
            } else {
                int start = -1; // unordered
                if (name == "ol") {
                    const std::string value = attribute(tag.attributes, "start");
                    start = value.empty() || !isDigit(value[0]) ? 1 : std::atoi(value.c_str());
                }
                lists_.push_back(start);
            }
            return;
        }
        if (name == "li") {
            endBlock();
            if (!tag.closing) {
                startBlock(BlockType::ListItem);
                current_.level = std::max(0, static_cast<int>(lists_.size()) - 1);
                if (!lists_.empty() && lists_.back() >= 0)
                    current_.marker = std::to_string(lists_.back()++) + ".";
            }
            return;
        }
        if (name == "summary" || name == "dt") {
            endBlock();
            int& bold = counts_[static_cast<size_t>(bitIndex(kBold))];
            if (!tag.closing) {
                startBlock(BlockType::Paragraph);
                ++bold;
            } else {
                bold = std::max(0, bold - 1);
            }
            return;
        }
        if (oneOf(name, kParagraphTags)) {
            // <li><p>text</p></li>: keep the list item instead of starting a paragraph.
            const bool emptyItem = open_ && current_.type == BlockType::ListItem && current_.spans.empty() && text_.empty();
            if (!(emptyItem && !tag.closing))
                endBlock();
            return;
        }
        // Anything else (span, sub, sup, mark, font, ...) is ignored; its text stays.
    }

    std::vector<Block>& out_;
    size_t sourceLine_;
    int baseQuote_;

    Block current_;
    bool open_ = false;
    std::string text_;
    uint8_t textStyle_ = 0;
    bool pendingSpace_ = false;
    uint8_t spaceStyle_ = 0; // style where the pending whitespace appeared
    std::array<int, 8> counts_{}; // open inline tags per style bit

    int quote_ = 0;
    std::vector<int> lists_; // next number for <ol>, -1 for <ul>

    bool inPre_ = false;
    std::string pre_;

    int skipDepth_ = 0;
    std::string skipTag_;

    bool inTable_ = false;
    bool inCaption_ = false;
    bool inHead_ = false;
    size_t rowHeaderCells_ = 0;
    Block table_;
};

} // namespace

size_t parseTag(std::string_view s, size_t pos, Tag& tag)
{
    tag = Tag{};
    const std::string_view rest = s.substr(pos);
    const auto until = [&](std::string_view end, size_t from) -> size_t {
        const size_t at = rest.find(end, from);
        return at == std::string_view::npos ? 0 : at + end.size();
    };
    if (rest.starts_with("<!--"))
        return until("-->", 4);
    if (rest.starts_with("<![CDATA["))
        return until("]]>", 9);
    if (rest.starts_with("<!") || rest.starts_with("<?"))
        return rest.size() > 2 && (isAlpha(rest[2]) || rest[1] == '?') ? until(">", 2) : 0;

    size_t i = 1;
    if (i < rest.size() && rest[i] == '/') {
        tag.closing = true;
        ++i;
    }
    if (i >= rest.size() || !isAlpha(rest[i]))
        return 0;
    const size_t nameStart = i;
    while (i < rest.size() && (isAlpha(rest[i]) || isDigit(rest[i]) || rest[i] == '-'))
        ++i;
    tag.name.assign(rest.substr(nameStart, i - nameStart));
    for (char& c : tag.name)
        c = lower(c);
    if (i < rest.size() && !isWhite(rest[i]) && rest[i] != '>' && rest[i] != '/')
        return 0; // "<a+b" is not a tag

    const size_t attrStart = i;
    char quote = 0;
    for (; i < rest.size(); ++i) {
        const char c = rest[i];
        if (quote) {
            if (c == quote)
                quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            break;
        } else if (c == '<' || (c == '\n' && i + 1 < rest.size() && rest[i + 1] == '\n')) {
            return 0;
        }
    }
    if (i >= rest.size())
        return 0;

    std::string_view attrs = trim(rest.substr(attrStart, i - attrStart));
    if (!attrs.empty() && attrs.back() == '/') {
        tag.selfClosing = true;
        attrs = trim(attrs.substr(0, attrs.size() - 1));
    }
    tag.attributes = attrs;
    static constexpr const char* kVoid[] = {"br", "hr", "img", "input", "meta", "link", "col", "area",
                                            "base", "wbr", "source", "track", "embed", "param"};
    if (oneOf(tag.name, kVoid))
        tag.selfClosing = true;
    return i + 1;
}

std::string attribute(std::string_view attrs, std::string_view name, bool* found)
{
    if (found)
        *found = false;
    size_t i = 0;
    while (i < attrs.size()) {
        while (i < attrs.size() && (isWhite(attrs[i]) || attrs[i] == '/'))
            ++i;
        const size_t nameStart = i;
        while (i < attrs.size() && !isWhite(attrs[i]) && attrs[i] != '=' && attrs[i] != '/')
            ++i;
        const std::string_view attrName = attrs.substr(nameStart, i - nameStart);
        while (i < attrs.size() && isWhite(attrs[i]))
            ++i;

        std::string_view value;
        if (i < attrs.size() && attrs[i] == '=') {
            ++i;
            while (i < attrs.size() && isWhite(attrs[i]))
                ++i;
            if (i < attrs.size() && (attrs[i] == '"' || attrs[i] == '\'')) {
                const char q = attrs[i++];
                const size_t end = std::min(attrs.find(q, i), attrs.size());
                value = attrs.substr(i, end - i);
                i = end + 1;
            } else {
                const size_t start = i;
                while (i < attrs.size() && !isWhite(attrs[i]))
                    ++i;
                value = attrs.substr(start, i - start);
            }
        }
        if (!attrName.empty() && equalsIgnoreCase(attrName, name)) {
            if (found)
                *found = true;
            std::string out;
            for (size_t k = 0; k < value.size();) {
                if (value[k] == '&') {
                    if (const size_t n = decodeEntity(value, k, out); n > 0) {
                        k += n;
                        continue;
                    }
                }
                out += value[k++];
            }
            return out;
        }
        if (attrName.empty())
            ++i;
    }
    return {};
}

size_t decodeEntity(std::string_view s, size_t pos, std::string& out)
{
    const std::string_view rest = s.substr(pos);
    if (rest.size() < 3 || rest[0] != '&')
        return 0;

    if (rest[1] == '#') {
        const bool hex = rest.size() > 2 && (rest[2] == 'x' || rest[2] == 'X');
        size_t i = hex ? 3 : 2;
        uint32_t cp = 0;
        const size_t digitsStart = i;
        while (i < rest.size() && i - digitsStart < 8) {
            const char c = rest[i];
            int digit = -1;
            if (isDigit(c))
                digit = c - '0';
            else if (hex && lower(c) >= 'a' && lower(c) <= 'f')
                digit = lower(c) - 'a' + 10;
            if (digit < 0)
                break;
            cp = cp * (hex ? 16u : 10u) + static_cast<uint32_t>(digit);
            ++i;
        }
        if (i == digitsStart || i >= rest.size() || rest[i] != ';')
            return 0;
        appendUtf8(out, cp);
        return i + 1;
    }

    size_t i = 1;
    while (i < rest.size() && i < 32 && (isAlpha(rest[i]) || isDigit(rest[i])))
        ++i;
    if (i == 1 || i >= rest.size() || rest[i] != ';')
        return 0;
    const std::string_view name = rest.substr(1, i - 1);
    for (const NamedEntity& e : kEntities) {
        if (name == e.name) {
            if (e.codepoint != 0xAD) // soft hyphen: invisible
                appendUtf8(out, e.codepoint);
            return i + 1;
        }
    }
    return 0;
}

uint8_t inlineTagStyle(std::string_view name)
{
    if (name == "b" || name == "strong")
        return kBold;
    if (name == "i" || name == "em" || name == "cite" || name == "var" || name == "dfn")
        return kItalic;
    if (name == "code" || name == "kbd" || name == "samp" || name == "tt")
        return kCode;
    if (name == "s" || name == "del" || name == "strike")
        return kStrike;
    if (name == "u" || name == "ins")
        return kUnderline;
    if (name == "a")
        return kLink;
    return 0;
}

Span imageSpan(const Tag& img, uint8_t style)
{
    Span span;
    span.text = attribute(img.attributes, "alt");
    if (span.text.empty())
        span.text = "image";
    span.style = static_cast<uint8_t>(style | kImage);
    span.url = attribute(img.attributes, "src");
    // Pixel sizes only ("120" or "120px"); percentages are ignored.
    const auto pixels = [&](std::string_view name) {
        const std::string value = attribute(img.attributes, name);
        size_t i = 0;
        int n = 0;
        while (i < value.size() && isDigit(value[i]) && n < 100000)
            n = n * 10 + (value[i++] - '0');
        const std::string_view unit = std::string_view(value).substr(i);
        return (unit.empty() || unit == "px") ? n : 0;
    };
    span.width = pixels("width");
    span.height = pixels("height");
    return span;
}

void parseHtml(std::string_view html, size_t sourceLine, int quote, std::vector<Block>& out)
{
    Converter(out, sourceLine, quote).run(html);
}

HtmlBlockStart htmlBlockStart(std::string_view line, std::string& endMarker)
{
    size_t indent = 0;
    while (indent < line.size() && indent < 4 && line[indent] == ' ')
        ++indent;
    if (indent > 3 || indent >= line.size() || line[indent] != '<')
        return HtmlBlockStart::None;
    const std::string_view s = line.substr(indent);

    if (s.starts_with("<!--")) {
        endMarker = "-->";
        return HtmlBlockStart::UntilComment;
    }
    if (s.starts_with("<![CDATA[")) {
        endMarker = "]]>";
        return HtmlBlockStart::UntilComment;
    }
    if (s.starts_with("<?")) {
        endMarker = "?>";
        return HtmlBlockStart::UntilComment;
    }
    if (s.size() > 2 && s[1] == '!' && isAlpha(s[2])) {
        endMarker = ">";
        return HtmlBlockStart::UntilComment;
    }

    for (const char* raw : {"pre", "script", "style", "textarea"}) {
        const size_t n = std::strlen(raw);
        if (startsWithIgnoreCase(s.substr(1), raw)
            && (s.size() == n + 1 || isWhite(s[n + 1]) || s[n + 1] == '>')) {
            endMarker = std::string("</") + raw + ">";
            return HtmlBlockStart::UntilTag;
        }
    }

    // <tag or </tag, then whitespace, '>', "/>" or end of line.
    size_t i = s.size() > 1 && s[1] == '/' ? 2 : 1;
    const size_t nameStart = i;
    while (i < s.size() && (isAlpha(s[i]) || isDigit(s[i])))
        ++i;
    std::string name(s.substr(nameStart, i - nameStart));
    for (char& c : name)
        c = lower(c);
    const bool boundary = i == s.size() || isWhite(s[i]) || s[i] == '>' || s.substr(i).starts_with("/>");
    if (!name.empty() && boundary && oneOf(name, kBlockTags))
        return HtmlBlockStart::UntilBlank;

    // Type 7: a complete open or close tag alone on the line.
    Tag tag;
    const size_t n = parseTag(s, 0, tag);
    if (n > 0 && !tag.name.empty() && trim(s.substr(n)).empty())
        return HtmlBlockStart::LoneTag;
    return HtmlBlockStart::None;
}

} // namespace md
