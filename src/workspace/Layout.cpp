#include "Layout.h"

#include "config/Ccl.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace {

bool typeFromName(std::string_view name, WorkspaceType& out)
{
    for (const WorkspaceType type : kWorkspaceTypes) {
        if (name == workspaceTypeName(type)) {
            out = type;
            return true;
        }
    }
    // Names the plan used before the reader and web tiles took several kinds each.
    if (name == "pdf" || name == "epub") {
        out = WorkspaceType::Reader;
        return true;
    }
    if (name == "html") {
        out = WorkspaceType::Web;
        return true;
    }
    return false;
}

// "50%" or "50": a share of the parent, in (0, 100].
bool parseSize(std::string_view text, float& out)
{
    if (!text.empty() && text.back() == '%')
        text.remove_suffix(1);
    while (!text.empty() && text.back() == ' ')
        text.remove_suffix(1);
    float value = 0.0f;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size() || !(value > 0.0f) || value > 100.0f)
        return false;
    out = value;
    return true;
}

// A split's single child takes its place (keeping the split's size).
void collapse(LayoutNode& node)
{
    for (LayoutNode& child : node.children)
        collapse(child);
    if (!node.tile && node.children.size() == 1) {
        const float size = node.size;
        LayoutNode only = std::move(node.children[0]);
        node = std::move(only);
        node.size = size;
    }
}

bool parseSplit(std::string_view body, size_t firstLine, LayoutNode& out, const std::string& where,
                std::vector<std::string>& warnings)
{
    std::vector<ccl::Entry> entries;
    ccl::Error error;
    if (!ccl::parseEntries(body, entries, &error, firstLine)) {
        warnings.push_back(where + ", line " + std::to_string(error.line) + ": " + error.message);
        return false;
    }
    out.tile = false;
    for (const ccl::Entry& e : entries) {
        const std::string at = where + " / " + e.key;
        WorkspaceType type{};
        if (e.key == "/") {
            continue;
        } else if (e.key == "split") {
            if (e.value == "horizontal")
                out.horizontal = true;
            else if (e.value == "vertical")
                out.horizontal = false;
            else
                warnings.push_back(at + ": expected horizontal or vertical, got \"" + e.value + "\"");
        } else if (e.key == "size") {
            if (!parseSize(e.value, out.size))
                warnings.push_back(at + ": expected a size like 30%, got \"" + e.value + "\"");
        } else if (typeFromName(e.key, type)) {
            LayoutNode tile;
            tile.tile = true;
            tile.type = type;
            if (e.nested)
                warnings.push_back(at + ": a tile takes a size (like 50%), not a block");
            else if (!e.value.empty() && !parseSize(e.value, tile.size))
                warnings.push_back(at + ": expected a size like 50%, got \"" + e.value + "\"");
            out.children.push_back(tile);
        } else if (e.nested) {
            LayoutNode split;
            if (parseSplit(e.value, e.line + 1, split, at, warnings)) {
                if (split.children.empty())
                    warnings.push_back(at + ": no tiles in this block");
                else
                    out.children.push_back(std::move(split));
            }
        } else {
            warnings.push_back(at
                               + ": unknown workspace type (expected editor, preview, outline, terminal, reader, web "
                                 "or audio)");
        }
    }
    return true;
}

void appendSize(std::string& out, float size)
{
    out += std::to_string(static_cast<int>(std::lround(size))) + "%";
}

void writeSplit(const LayoutNode& node, int indent, std::string& out)
{
    const std::string pad(static_cast<size_t>(indent), ' ');
    out += pad + "split = " + (node.horizontal ? "horizontal" : "vertical") + "\n";
    for (size_t i = 0; i < node.children.size(); ++i) {
        const LayoutNode& child = node.children[i];
        // The last child takes what is left, so the sizes always add up.
        const bool sized = i + 1 < node.children.size() && child.size > 0.0f;
        if (child.tile) {
            out += pad + workspaceTypeName(child.type) + " =";
            if (sized) {
                out += ' ';
                appendSize(out, child.size);
            }
            out += '\n';
        } else {
            out += pad + "group =\n";
            if (sized) {
                out += pad + "  size = ";
                appendSize(out, child.size);
                out += '\n';
            }
            writeSplit(child, indent + 2, out);
        }
    }
}

struct Line
{
    std::string_view text; // without the line break
    size_t indent = 0;
    bool blank = true;
};

std::vector<Line> splitLines(std::string_view text)
{
    std::vector<Line> lines;
    for (size_t pos = 0; pos < text.size();) {
        const size_t newline = std::min(text.find('\n', pos), text.size());
        Line line;
        line.text = text.substr(pos, newline - pos);
        if (!line.text.empty() && line.text.back() == '\r')
            line.text.remove_suffix(1);
        while (line.indent < line.text.size() && (line.text[line.indent] == ' ' || line.text[line.indent] == '\t'))
            ++line.indent;
        line.blank = line.indent == line.text.size();
        lines.push_back(line);
        pos = newline + 1;
    }
    return lines;
}

// "key = value" with this key and an empty value (the start of a block).
bool isBlockStart(const Line& line, std::string_view key)
{
    if (line.blank)
        return false;
    std::string_view s = line.text.substr(line.indent);
    const size_t eq = s.find('=');
    if (eq == std::string_view::npos)
        return false;
    std::string_view k = s.substr(0, eq);
    std::string_view v = s.substr(eq + 1);
    while (!k.empty() && (k.back() == ' ' || k.back() == '\t'))
        k.remove_suffix(1);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r'))
        v.remove_prefix(1);
    return k == key && v.empty();
}

// End (exclusive) of the block whose key is lines[start]: the lines indented
// deeper, not counting blank lines at its end.
size_t blockEnd(const std::vector<Line>& lines, size_t start)
{
    size_t end = start + 1;
    for (size_t i = start + 1; i < lines.size(); ++i) {
        if (lines[i].blank)
            continue;
        if (lines[i].indent <= lines[start].indent)
            break;
        end = i + 1;
    }
    return end;
}

} // namespace

const char* workspaceTypeLabel(WorkspaceType type)
{
    switch (type) {
    case WorkspaceType::Editor:
        return "Editor";
    case WorkspaceType::Preview:
        return "Preview";
    case WorkspaceType::Outline:
        return "Outline";
    case WorkspaceType::Terminal:
        return "Terminal";
    case WorkspaceType::Reader:
        return "Reader";
    case WorkspaceType::Web:
        return "Web";
    case WorkspaceType::Audio:
        return "Audio";
    }
    return "";
}

const char* workspaceTypeName(WorkspaceType type)
{
    switch (type) {
    case WorkspaceType::Editor: return "editor";
    case WorkspaceType::Preview: return "preview";
    case WorkspaceType::Outline: return "outline";
    case WorkspaceType::Terminal: return "terminal";
    case WorkspaceType::Reader: return "reader";
    case WorkspaceType::Web: return "web";
    case WorkspaceType::Audio: return "audio";
    }
    return "editor";
}

void parseLayouts(std::string_view text, std::vector<Layout>& layouts, std::vector<std::string>& warnings)
{
    std::vector<ccl::Entry> top;
    if (!ccl::parseEntries(text, top, nullptr))
        return; // the settings reader reports syntax errors
    for (const ccl::Entry& section : top) {
        if (section.key != "layouts" || !section.nested)
            continue;
        std::vector<ccl::Entry> entries;
        ccl::Error error;
        if (!ccl::parseEntries(section.value, entries, &error, section.line + 1)) {
            warnings.push_back("layouts, line " + std::to_string(error.line) + ": " + error.message);
            continue;
        }
        for (const ccl::Entry& e : entries) {
            if (e.key == "/")
                continue;
            const std::string where = "layouts / " + e.key;
            if (!e.nested) {
                warnings.push_back(where + ": expected its tiles in an indented block below it");
                continue;
            }
            Layout layout;
            layout.name = e.key;
            if (!parseSplit(e.value, e.line + 1, layout.root, where, warnings))
                continue;
            if (layout.root.children.empty()) {
                warnings.push_back(where + ": no tiles, so it was skipped");
                continue;
            }
            collapse(layout.root);
            layout.root.size = -1.0f;
            auto same = std::find_if(layouts.begin(), layouts.end(), [&](const Layout& l) { return l.name == e.key; });
            if (same != layouts.end())
                *same = std::move(layout);
            else
                layouts.push_back(std::move(layout));
        }
    }
}

std::string layoutToCcl(const LayoutNode& root, int indent)
{
    std::string out;
    if (root.tile)
        out += std::string(static_cast<size_t>(indent), ' ') + workspaceTypeName(root.type) + " =\n";
    else
        writeSplit(root, indent, out);
    return out;
}

std::string replaceLayoutBlock(std::string_view settings, std::string_view name, const LayoutNode& root)
{
    const std::vector<Line> lines = splitLines(settings);
    // Keep the file's own line endings.
    const std::string eol = settings.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
    const auto withEol = [&](const std::string& text) {
        if (eol == "\n")
            return text;
        std::string out;
        for (const char c : text) {
            if (c == '\n')
                out += '\r';
            out += c;
        }
        return out;
    };
    const auto join = [&](size_t from, size_t to) {
        std::string out;
        for (size_t i = from; i < to; ++i) {
            out += lines[i].text;
            out += eol;
        }
        return out;
    };

    // The last top-level `layouts =` section.
    size_t section = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].indent == 0 && isBlockStart(lines[i], "layouts"))
            section = i;
    }
    if (section == lines.size()) {
        std::string out(settings);
        if (!out.empty() && out.back() != '\n')
            out += eol;
        out += withEol("\nlayouts =\n  " + std::string(name) + " =\n" + layoutToCcl(root, 4));
        return out;
    }

    const size_t sectionEnd = blockEnd(lines, section);
    size_t childIndent = 2;
    for (size_t i = section + 1; i < sectionEnd; ++i) {
        if (!lines[i].blank) {
            childIndent = lines[i].indent;
            break;
        }
    }
    const std::string block = std::string(childIndent, ' ') + std::string(name) + " =\n"
                            + layoutToCcl(root, static_cast<int>(childIndent) + 2);

    for (size_t i = section + 1; i < sectionEnd; ++i) {
        if (lines[i].indent == childIndent && isBlockStart(lines[i], name)) {
            return join(0, i) + withEol(block) + join(blockEnd(lines, i), lines.size());
        }
    }
    return join(0, sectionEnd) + withEol(block) + join(sectionEnd, lines.size());
}

const Layout* findLayout(const std::vector<Layout>& layouts, std::string_view name)
{
    for (const Layout& layout : layouts) {
        if (layout.name == name)
            return &layout;
    }
    return nullptr;
}
