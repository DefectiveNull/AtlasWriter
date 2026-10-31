#include "MarkdownView.h"

#include "text/Font.h"
#include "text/TextRenderer.h"
#include "text/Utf8.h"
#include "ui/ImageCache.h"
#include "ui/Draw.h"
#include "ui/Keys.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace {

constexpr float kMaxColumnEms = 40.0f;
constexpr float kWheelLines = 3.0f;
constexpr float kBodySpacing = 1.4f;    // line height multiplier for running text
constexpr float kHeadingSpacing = 1.15f;
constexpr float kCodeSpacing = 1.25f;

constexpr const char* kBullets[] = {"\xE2\x80\xA2", "\xE2\x80\x93"}; // bullet, en dash

// Kerning between the end of `a` and the start of `b`, so pieces measured
// separately line up exactly with the merged run that gets drawn.
float seamKern(const Font& font, std::string_view a, std::string_view b)
{
    if (a.empty() || b.empty() || !font.kerning())
        return 0.0f;
    size_t start = a.size() - 1;
    while (start > 0 && utf8::isContinuation(a[start]))
        --start;
    const char* p = a.data() + start;
    const uint32_t left = utf8::decode(p, a.data() + a.size());
    p = b.data();
    const uint32_t right = utf8::decode(p, b.data() + b.size());
    if (left == '\t' || right == '\t')
        return 0.0f;
    return font.kern(left, right);
}

bool sameColor(const SDL_FColor& a, const SDL_FColor& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

SDL_FColor colorFor(const md::Block& block, uint8_t style)
{
    if (style & md::kLink)
        return theme::kLink;
    if (style & md::kImage)
        return theme::kMuted;
    if (style & md::kCode)
        return theme::kCodeText;
    if (block.type == md::BlockType::Heading)
        return theme::kHeading;
    if (block.task == 1)
        return theme::kMuted;
    if (block.quote > 0)
        return theme::kQuoteText;
    return theme::kText;
}

} // namespace

void MarkdownView::setFonts(const MarkdownFonts& fonts)
{
    fonts_ = fonts;
    invalidateLayout();
}

void MarkdownView::setImages(ImageCache* images, std::string baseDirectory, float scale)
{
    images_ = images;
    baseDirectory_ = std::move(baseDirectory);
    imageScale_ = scale;
    invalidateLayout();
}

void MarkdownView::setSource(std::string_view text, uint64_t version)
{
    if (version == version_)
        return;
    version_ = version;
    blocks_ = md::parse(text);
    buildHeadingKeys();
    invalidateLayout();
}

void MarkdownView::ensureLayout()
{
    if (layoutWidth_ != columnWidth())
        layout();
}

// ---------------------------------------------------------------------------
// Collapsible headings
// ---------------------------------------------------------------------------

void MarkdownView::buildHeadingKeys()
{
    // "level:text#n" - the n-th heading with this level and text - so edits
    // elsewhere in the document keep the same headings collapsed.
    headingKeys_.assign(blocks_.size(), {});
    std::unordered_map<std::string, int> seen;
    for (size_t i = 0; i < blocks_.size(); ++i) {
        const md::Block& block = blocks_[i];
        if (block.type != md::BlockType::Heading)
            continue;
        std::string key = std::to_string(block.level) + ':';
        for (const md::Span& span : block.spans)
            key += span.text;
        key += '#' + std::to_string(seen[key]++);
        headingKeys_[i] = std::move(key);
    }
}

int MarkdownView::headingAt(float x, float y) const
{
    const float left = columnLeft() - std::round(em() * 2.0f); // include the marker gutter
    if (x < left || x > columnLeft() + columnWidth() || !ui::contains(bounds_, x, y))
        return -1;
    const float docY = y - bounds_.y + std::round(scrollY_);
    for (size_t i = 0; i < headings_.size(); ++i) {
        if (docY >= headings_[i].top && docY < headings_[i].bottom)
            return static_cast<int>(i);
    }
    return -1;
}

bool MarkdownView::isOverHeading(float x, float y) const
{
    return headingAt(x, y) >= 0;
}

void MarkdownView::toggle(size_t block)
{
    // Keep the clicked heading at the same place on screen.
    float screenTop = 0.0f;
    for (const HeadingHit& h : headings_) {
        if (h.block == block)
            screenTop = h.top - scrollY_;
    }
    const std::string& key = headingKeys_[block];
    if (!collapsed_.erase(key))
        collapsed_.insert(key);
    layout();
    for (size_t i = 0; i < headings_.size(); ++i) {
        if (headings_[i].block == block) {
            scrollY_ = headings_[i].top - screenTop;
            hoverHeading_ = static_cast<int>(i);
        }
    }
    clampScroll();
}

size_t MarkdownView::topSourceLine()
{
    ensureLayout();
    const float sy = std::round(scrollY_) + 1.0f;
    for (const Line& line : lines_) {
        if (line.y + line.height > sy)
            return line.sourceLine;
    }
    return lines_.empty() ? 0 : lines_.back().sourceLine;
}

void MarkdownView::revealSourceLine(size_t sourceLine)
{
    // The target block: the last one starting at or before the line.
    size_t target = 0;
    for (size_t i = 0; i < blocks_.size() && blocks_[i].sourceLine <= sourceLine; ++i)
        target = i;
    if (!blocks_.empty()) {
        // Walking back, a heading is an ancestor (and hides the target when
        // collapsed) if every heading in between is of a lower level.
        int minLevel = blocks_[target].type == md::BlockType::Heading ? blocks_[target].level : 7;
        bool changed = false;
        for (size_t i = target; i-- > 0;) {
            const md::Block& block = blocks_[i];
            if (block.type != md::BlockType::Heading || block.level >= minLevel)
                continue;
            minLevel = block.level;
            changed |= collapsed_.erase(headingKeys_[i]) > 0;
        }
        if (changed)
            invalidateLayout();
    }
    ensureLayout();
    const auto it = std::find_if(lines_.begin(), lines_.end(),
                                 [&](const Line& line) { return line.sourceLine >= sourceLine; });
    scrollY_ = it == lines_.end() ? contentHeight_ : it->y - std::round(bodyLineHeight() * 0.5f);
    clampScroll();
}

void MarkdownView::collapseAll()
{
    for (const std::string& key : headingKeys_) {
        if (!key.empty())
            collapsed_.insert(key);
    }
    invalidateLayout();
}

void MarkdownView::expandAll()
{
    collapsed_.clear();
    invalidateLayout();
}

void MarkdownView::drawMarker(SDL_Renderer* renderer, float cx, float cy, bool collapsed, SDL_FColor color) const
{
    // A small triangle: pointing right when collapsed, down when expanded.
    const float r = std::round(em() * 0.28f);
    SDL_Vertex v[3];
    if (collapsed) {
        v[0].position = {cx - r * 0.6f, cy - r};
        v[1].position = {cx + r * 0.8f, cy};
        v[2].position = {cx - r * 0.6f, cy + r};
    } else {
        v[0].position = {cx - r, cy - r * 0.6f};
        v[1].position = {cx + r, cy - r * 0.6f};
        v[2].position = {cx, cy + r * 0.8f};
    }
    for (SDL_Vertex& vertex : v) {
        vertex.color = color;
        vertex.tex_coord = {0.0f, 0.0f};
    }
    SDL_RenderGeometry(renderer, nullptr, v, 3, nullptr, 0);
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

std::string MarkdownView::resolveImage(const std::string& url) const
{
    std::string_view u = url;
    u = u.substr(0, std::min(u.find_first_of("?#"), u.size()));
    if (u.empty() || u.starts_with("data:"))
        return {};
    if (u.starts_with("file://")) {
        u.remove_prefix(7);
        if (u.size() > 2 && u[0] == '/' && u[2] == ':') // file:///C:/...
            u.remove_prefix(1);
    } else if (u.find("://") != std::string_view::npos) {
        return {}; // remote images are not fetched
    }

    std::string path;
    for (size_t i = 0; i < u.size(); ++i) { // %20 and friends
        const auto hex = [](char c) {
            return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;
        };
        if (u[i] == '%' && i + 2 < u.size() && hex(u[i + 1]) >= 0 && hex(u[i + 2]) >= 0) {
            path += static_cast<char>(hex(u[i + 1]) * 16 + hex(u[i + 2]));
            i += 2;
        } else {
            path += u[i];
        }
    }
    const bool absolute = path[0] == '/' || path[0] == '\\' || (path.size() > 1 && path[1] == ':');
    return absolute ? path : baseDirectory_ + path;
}

// Display size of a loaded image, scaled to fit `maxWidth`. False while it
// is loading or when it can't be shown (then the alt text is used).
bool MarkdownView::imageSize(const md::Span& span, float maxWidth, SDL_Texture*& texture, float& w, float& h)
{
    if (!images_ || span.url.empty())
        return false;
    const std::string path = resolveImage(span.url);
    if (path.empty())
        return false;
    const ImageCache::Image* image = images_->request(path);
    if (image->state != ImageCache::State::Ready || image->width <= 0 || image->height <= 0)
        return false;

    const float aspect = static_cast<float>(image->height) / static_cast<float>(image->width);
    if (span.width > 0 && span.height > 0) {
        w = static_cast<float>(span.width) * imageScale_;
        h = static_cast<float>(span.height) * imageScale_;
    } else if (span.width > 0) {
        w = static_cast<float>(span.width) * imageScale_;
        h = w * aspect;
    } else if (span.height > 0) {
        h = static_cast<float>(span.height) * imageScale_;
        w = h / aspect;
    } else {
        w = static_cast<float>(image->width) * imageScale_;
        h = static_cast<float>(image->height) * imageScale_;
    }
    if (w > maxWidth) {
        h *= maxWidth / w;
        w = maxWidth;
    }
    w = std::max(1.0f, std::round(w));
    h = std::max(1.0f, std::round(h));
    texture = image->texture;
    return true;
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

float MarkdownView::bodyLineHeight() const
{
    return static_cast<float>(fonts_.regular->font().lineHeight());
}

float MarkdownView::em() const
{
    return fonts_.regular->font().pixelSize();
}

float MarkdownView::columnWidth() const
{
    const float available = std::max(1.0f, bounds_.w - 2.0f * std::round(em() * 1.5f));
    return std::round(std::min(available, kMaxColumnEms * em()));
}

float MarkdownView::columnLeft() const
{
    return std::round(bounds_.x + (bounds_.w - columnWidth()) * 0.5f);
}

TextRenderer* MarkdownView::fontFor(const md::Block& block, uint8_t style) const
{
    if (style & md::kCode)
        return fonts_.code;
    if (block.type == md::BlockType::Heading)
        return fonts_.heading[std::clamp(block.level, 1, 6) - 1];
    const bool bold = (style & md::kBold) != 0;
    const bool italic = (style & (md::kItalic | md::kImage)) != 0;
    if (bold && italic)
        return fonts_.boldItalic;
    if (bold)
        return fonts_.bold;
    return italic ? fonts_.italic : fonts_.regular;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void MarkdownView::layout()
{
    lines_.clear();
    maxLineHeight_ = 0.0f;
    boxes_.clear();
    headings_.clear();
    hoverHeading_ = -1;
    layoutWidth_ = columnWidth();

    // Skip what collapsed headings hide: everything up to the next heading
    // of the same or a higher level.
    std::vector<size_t> visible;
    visible.reserve(blocks_.size());
    for (size_t i = 0; i < blocks_.size();) {
        const md::Block& block = blocks_[i];
        visible.push_back(i++);
        if (block.type == md::BlockType::Heading && collapsed_.count(headingKeys_[visible.back()])) {
            while (i < blocks_.size() && !(blocks_[i].type == md::BlockType::Heading && blocks_[i].level <= block.level))
                ++i;
        }
    }

    float y = std::round(bodyLineHeight());
    for (size_t k = 0; k < visible.size(); ++k) {
        const md::Block& block = blocks_[visible[k]];
        const size_t firstLine = lines_.size();
        layoutBlock(block, k + 1 < visible.size() ? &blocks_[visible[k + 1]] : nullptr, y);
        if (block.type == md::BlockType::Heading && firstLine < lines_.size()) {
            const Line& first = lines_[firstLine];
            const Line& last = lines_.back();
            const float ascent = static_cast<float>(fontFor(block, 0)->font().ascent());
            headings_.push_back({first.y, last.y + last.height,
                                 std::round(first.y + first.baseline - ascent * 0.35f), visible[k]});
        }
    }
    contentHeight_ = y + bodyLineHeight();
}

void MarkdownView::layoutBlock(const md::Block& block, const md::Block* next, float& y)
{
    const float lh = bodyLineHeight();
    const float quoteIndent = std::round(em() * 1.2f);
    const float gap = std::round(lh * 0.7f);
    const float left = static_cast<float>(block.quote) * quoteIndent;
    const float right = layoutWidth_;
    const float top = y;

    switch (block.type) {
    case md::BlockType::Heading: {
        if (y > lh * 1.5f)
            y += std::round(lh * 0.8f);
        flow(block, left, right, y, kHeadingSpacing);
        if (block.level <= 2) {
            y += std::round(lh * 0.15f);
            boxes_.push_back({{left, y, right - left, 1.0f}, theme::kRule});
        }
        y += std::round(lh * 0.5f);
        break;
    }

    case md::BlockType::Paragraph:
        flow(block, left, right, y, kBodySpacing);
        y += gap;
        break;

    case md::BlockType::ListItem: {
        const float indent = left + std::round(static_cast<float>(block.level + 1) * em() * 1.6f);
        const size_t firstLine = lines_.size();
        flow(block, indent, right, y, kBodySpacing);
        if (firstLine < lines_.size()) {
            Line& line = lines_[firstLine];
            TextRenderer* text = fonts_.regular;
            const float markerGap = std::round(em() * 0.5f);
            if (block.task >= 0) {
                // Checkbox: an outlined square, filled when done.
                const float side = std::round(em() * 0.75f);
                const SDL_FRect box{indent - markerGap - side, line.y + line.baseline - side, side, side};
                boxes_.push_back({box, block.task == 1 ? theme::kMenuActive : theme::kMuted});
                if (block.task == 0)
                    boxes_.push_back({{box.x + 1.0f, box.y + 1.0f, box.w - 2.0f, box.h - 2.0f}, theme::kEditorBg});
            } else {
                std::string marker = block.marker.empty() ? kBullets[block.level % 2] : block.marker;
                const float width = text->measure(marker);
                line.runs.push_back({text, indent - markerGap - width, width, std::move(marker),
                                     theme::kMuted, 0, false});
            }
        }
        const bool tight = next && next->type == md::BlockType::ListItem;
        y += tight ? std::round(lh * 0.2f) : gap;
        break;
    }

    case md::BlockType::Code: {
        TextRenderer* text = fonts_.code;
        const Font& font = text->font();
        const float pad = std::round(em() * 0.7f);
        const float rowHeight = std::round(static_cast<float>(font.lineHeight()) * kCodeSpacing);
        const float baseline = std::round((rowHeight - static_cast<float>(font.lineHeight())) * 0.5f)
                             + static_cast<float>(font.ascent());
        boxes_.push_back({{left, y, right - left, 0.0f}, theme::kCodeBg});
        const size_t boxIndex = boxes_.size() - 1;
        y += pad;

        std::string_view code = block.code;
        size_t lineNo = block.sourceLine + 1;
        while (true) {
            const size_t newline = std::min(code.find('\n'), code.size());
            Line line;
            line.y = y;
            line.height = rowHeight;
            line.baseline = baseline;
            line.sourceLine = lineNo++;
            const std::string_view piece = code.substr(0, newline);
            if (!piece.empty())
                line.runs.push_back({text, left + pad, text->measure(piece), std::string(piece),
                                     theme::kCodeText, 0, false});
            maxLineHeight_ = std::max(maxLineHeight_, rowHeight);
            lines_.push_back(std::move(line));
            y += rowHeight;
            if (newline >= code.size())
                break;
            code.remove_prefix(newline + 1);
        }
        y += pad;
        boxes_[boxIndex].rect.h = y - boxes_[boxIndex].rect.y;
        y += gap;
        break;
    }

    case md::BlockType::Rule:
        y += std::round(lh * 0.4f);
        boxes_.push_back({{left, y, right - left, std::max(1.0f, std::round(lh / 16.0f))}, theme::kRule});
        y += std::round(lh * 0.4f) + gap;
        break;

    case md::BlockType::Table:
        layoutTable(block, left, right, y);
        y += gap;
        break;
    }

    // Quote bars, joined across consecutive quoted blocks.
    for (int depth = 1; depth <= block.quote; ++depth) {
        const float x = static_cast<float>(depth - 1) * quoteIndent;
        const bool continues = next && next->quote >= depth;
        const float bottom = continues ? y : y - gap;
        boxes_.push_back({{x, top, std::max(2.0f, std::round(em() * 0.18f)), bottom - top}, theme::kQuoteBar});
    }
}

// Word-wraps a block's spans into lines between `left` and `right`.
void MarkdownView::flow(const md::Block& block, float left, float right, float& y, float spacing)
{
    TextRenderer* base = fontFor(block, 0);
    Line line;
    line.sourceLine = block.sourceLine;
    float x = left;

    const auto newLine = [&] {
        finishLine(line, y, spacing, base);
        line = Line{};
        line.sourceLine = block.sourceLine;
        x = left;
    };

    for (const md::Span& span : block.spans) {
        TextRenderer* text = fontFor(block, span.style);
        const SDL_FColor color = colorFor(block, span.style);
        const uint8_t decoration = static_cast<uint8_t>(((span.style & (md::kLink | md::kUnderline)) ? kUnderline : 0)
                                                        | ((span.style & md::kStrike) ? kStrikeThrough : 0));
        const bool code = (span.style & md::kCode) != 0;
        const float space = text->advance(' ');

        const auto append = [&](std::string_view str, float width) {
            if (!line.runs.empty()) {
                Run& last = line.runs.back();
                if (last.text == text && sameColor(last.color, color) && last.decoration == decoration
                    && last.codeBackground == code && !last.image && std::abs(last.x + last.width - x) < 0.01f) {
                    const float seam = seamKern(text->font(), last.str, str);
                    last.str.append(str);
                    last.width += seam + width;
                    x += seam + width;
                    return;
                }
            }
            line.runs.push_back({text, x, width, std::string(str), color, decoration, code});
            x += width;
        };

        if (span.style & md::kImage) {
            SDL_Texture* texture = nullptr;
            float w = 0.0f, h = 0.0f;
            if (imageSize(span, right - left, texture, w, h)) {
                if (x + w > right && x > left)
                    newLine();
                line.runs.push_back({text, x, w, {}, color, 0, false, texture, h});
                x += w;
                continue;
            }
            // Not loaded (yet), missing or remote: fall back to the alt text.
        }

        const std::string_view s = span.text;
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] == '\n') {
                newLine();
                ++i;
                continue;
            }
            if (s[i] == ' ' || s[i] == '\t') {
                size_t end = i;
                while (end < s.size() && (s[end] == ' ' || s[end] == '\t'))
                    ++end;
                if (x > left) // no leading spaces on wrapped lines
                    append(std::string(end - i, ' '), space * static_cast<float>(end - i));
                i = end;
                continue;
            }

            size_t end = i;
            while (end < s.size() && s[end] != ' ' && s[end] != '\t' && s[end] != '\n')
                ++end;
            std::string_view word = s.substr(i, end - i);
            i = end;

            float width = text->measure(word);
            if (x + width > right && x > left)
                newLine();
            // A word wider than the column: break it between codepoints.
            while (x + width > right && !word.empty()) {
                const size_t n = text->fit(word, right - x);
                if (n >= word.size())
                    break;
                append(word.substr(0, n), text->measure(word.substr(0, n)));
                newLine();
                word.remove_prefix(n);
                width = text->measure(word);
            }
            if (!word.empty())
                append(word, width);
        }
    }
    finishLine(line, y, spacing, base);
}

void MarkdownView::finishLine(Line& line, float& y, float spacing, TextRenderer* base)
{
    // Mixed fonts share a baseline: tallest ascent above it, deepest descent below.
    // Images sit on the baseline and push the line taller, without the
    // text's line spacing being applied to their height.
    float above = static_cast<float>(base->font().ascent());
    float below = static_cast<float>(base->font().lineHeight() - base->font().ascent());
    float imageAbove = 0.0f;
    for (const Run& run : line.runs) {
        if (run.image) {
            imageAbove = std::max(imageAbove, run.imageHeight);
            continue;
        }
        const Font& font = run.text->font();
        above = std::max(above, static_cast<float>(font.ascent()));
        below = std::max(below, static_cast<float>(font.lineHeight() - font.ascent()));
    }
    const float natural = above + below;
    const float textHeight = std::round(natural * spacing);
    const float extra = std::max(0.0f, imageAbove - above);
    line.y = y;
    line.height = textHeight + extra;
    line.baseline = std::round((textHeight - natural) * 0.5f + above) + extra;
    y += line.height;
    maxLineHeight_ = std::max(maxLineHeight_, line.height);
    lines_.push_back(std::move(line));
}

// ---------------------------------------------------------------------------
// Tables
// ---------------------------------------------------------------------------

namespace {

constexpr float kTableSpacing = 1.25f;

md::Block cellBlock(const md::Block& table, const std::vector<md::Span>& spans, bool header, size_t sourceLine)
{
    md::Block cell;
    cell.type = md::BlockType::Paragraph;
    cell.quote = table.quote;
    cell.sourceLine = sourceLine;
    cell.spans = spans;
    if (header) {
        for (md::Span& span : cell.spans)
            span.style |= md::kBold;
    }
    return cell;
}

} // namespace

// Widest line of a cell when nothing wraps, and its widest single word
// (the narrowest the column can get without breaking words).
void MarkdownView::measureCell(const md::Block& cell, float& natural, float& minimum)
{
    natural = minimum = 0.0f;
    float line = 0.0f;
    for (const md::Span& span : cell.spans) {
        SDL_Texture* texture = nullptr;
        float w = 0.0f, h = 0.0f;
        if ((span.style & md::kImage) && imageSize(span, layoutWidth_, texture, w, h)) {
            line += w;
            minimum = std::max(minimum, std::min(w, em() * 6.0f)); // images can shrink
            continue;
        }
        const TextRenderer* text = fontFor(cell, span.style);
        std::string_view s = span.text;
        while (!s.empty()) {
            const size_t newline = std::min(s.find('\n'), s.size());
            const std::string_view piece = s.substr(0, newline);
            line += text->measure(piece);
            size_t start = 0;
            for (size_t i = 0; i <= piece.size(); ++i) {
                if (i == piece.size() || piece[i] == ' ' || piece[i] == '\t') {
                    minimum = std::max(minimum, text->measure(piece.substr(start, i - start)));
                    start = i + 1;
                }
            }
            if (newline < s.size()) {
                natural = std::max(natural, line);
                line = 0.0f;
                s.remove_prefix(newline + 1);
            } else {
                s = {};
            }
        }
    }
    natural = std::max(natural, line);
}

void MarkdownView::layoutTable(const md::Block& table, float left, float right, float& y)
{
    size_t columns = table.align.size();
    for (const md::TableRow& row : table.rows)
        columns = std::max(columns, row.cells.size());
    if (columns == 0)
        return;

    const float padX = std::round(em() * 0.6f);
    const float padY = std::round(bodyLineHeight() * 0.25f);
    const float border = 1.0f;
    const float available = std::max(1.0f, right - left);

    // Column widths: natural if they fit; otherwise shrink the columns that
    // have slack toward their longest word; if even that is too wide,
    // scale the minimums down and let words break.
    std::vector<float> natural(columns, 0.0f), minimum(columns, 0.0f);
    for (size_t r = 0; r < table.rows.size(); ++r) {
        const md::TableRow& row = table.rows[r];
        for (size_t c = 0; c < row.cells.size(); ++c) {
            float n = 0.0f, m = 0.0f;
            measureCell(cellBlock(table, row.cells[c], row.header, 0), n, m);
            natural[c] = std::max(natural[c], n + 2.0f * padX);
            minimum[c] = std::max(minimum[c], m + 2.0f * padX);
        }
    }
    float sumNatural = 0.0f, sumMinimum = 0.0f;
    for (size_t c = 0; c < columns; ++c) {
        natural[c] = std::max(natural[c], 2.0f * padX + em());
        minimum[c] = std::min(std::max(minimum[c], 2.0f * padX + em()), natural[c]);
        sumNatural += natural[c];
        sumMinimum += minimum[c];
    }
    std::vector<float> width(columns);
    for (size_t c = 0; c < columns; ++c) {
        if (sumNatural <= available)
            width[c] = natural[c];
        else if (sumMinimum >= available)
            width[c] = minimum[c] * available / sumMinimum;
        else
            width[c] = minimum[c] + (natural[c] - minimum[c]) * (available - sumMinimum) / (sumNatural - sumMinimum);
        width[c] = std::floor(width[c]);
    }
    std::vector<float> columnX(columns + 1, left);
    for (size_t c = 0; c < columns; ++c)
        columnX[c + 1] = columnX[c] + width[c];
    const float tableWidth = columnX[columns] - left;

    const float top = y;
    size_t bodyRow = 0;
    for (size_t r = 0; r < table.rows.size(); ++r) {
        const md::TableRow& row = table.rows[r];
        const size_t sourceLine = table.sourceLine + (r == 0 ? 0 : r + 1); // + the delimiter row
        const float rowTop = y;
        const size_t backgroundIndex = boxes_.size();
        boxes_.push_back({{left, rowTop, tableWidth, 0.0f},
                          row.header ? theme::kTableHeaderBg : (bodyRow++ % 2 ? theme::kTableStripe : SDL_FColor{})});
        const size_t firstLine = lines_.size();

        float rowBottom = rowTop + 2.0f * padY;
        for (size_t c = 0; c < columns; ++c) {
            static const std::vector<md::Span> kEmpty;
            const md::Block cell = cellBlock(table, c < row.cells.size() ? row.cells[c] : kEmpty, row.header, sourceLine);
            const float x0 = columnX[c] + padX;
            const float x1 = std::max(x0 + 1.0f, columnX[c + 1] - padX);
            float cy = rowTop + padY;
            const size_t before = lines_.size();
            flow(cell, x0, x1, cy, kTableSpacing);

            const md::Align align = c < table.align.size() ? table.align[c] : md::Align::Default;
            if (align == md::Align::Center || align == md::Align::Right) {
                const float factor = align == md::Align::Center ? 0.5f : 1.0f;
                for (size_t l = before; l < lines_.size(); ++l) {
                    std::vector<Run>& runs = lines_[l].runs;
                    if (runs.empty())
                        continue;
                    const float used = runs.back().x + runs.back().width - x0;
                    const float shift = std::round(std::max(0.0f, (x1 - x0) - used) * factor);
                    for (Run& run : runs)
                        run.x += shift;
                }
            }
            rowBottom = std::max(rowBottom, cy + padY);
        }
        boxes_[backgroundIndex].rect.h = rowBottom - rowTop;
        if (boxes_[backgroundIndex].color.a == 0.0f)
            boxes_.erase(boxes_.begin() + static_cast<std::ptrdiff_t>(backgroundIndex));

        // Cells were laid out one after another; keep lines sorted by y.
        std::stable_sort(lines_.begin() + static_cast<std::ptrdiff_t>(firstLine), lines_.end(),
                         [](const Line& a, const Line& b) { return a.y < b.y; });

        // Rule above the row; the one under the header rows is stronger.
        const bool afterHeader = r > 0 && table.rows[r - 1].header && !row.header;
        boxes_.push_back({{left, rowTop, tableWidth, afterHeader ? 2.0f * border : border},
                          afterHeader ? theme::kTableHeaderRule : theme::kTableBorder});
        y = rowBottom;
    }
    boxes_.push_back({{left, y, tableWidth, border}, theme::kTableBorder});
    for (size_t c = 0; c <= columns; ++c)
        boxes_.push_back({{std::min(columnX[c], left + tableWidth - border), top, border, y - top + border},
                          theme::kTableBorder});
    y += border;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void MarkdownView::clampScroll()
{
    scrollY_ = std::clamp(scrollY_, 0.0f, std::max(0.0f, contentHeight_ - bounds_.h));
}

void MarkdownView::scrollToSourceLine(size_t sourceLine)
{
    if (layoutWidth_ != columnWidth())
        layout();
    const auto it = std::find_if(lines_.begin(), lines_.end(),
                                 [&](const Line& line) { return line.sourceLine >= sourceLine; });
    scrollY_ = it == lines_.end() ? contentHeight_ : it->y - std::round(bodyLineHeight());
    clampScroll();
}

bool MarkdownView::handleEvent(const SDL_Event& event)
{
    ensureLayout();
    const float lh = bodyLineHeight();
    const float page = std::max(lh, bounds_.h - 2.0f * lh);
    const float before = scrollY_;

    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        const int heading = headingAt(event.motion.x, event.motion.y);
        if (heading == hoverHeading_)
            return false;
        hoverHeading_ = heading;
        return true;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (hoverHeading_ < 0)
            return false;
        hoverHeading_ = -1;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (event.button.button != SDL_BUTTON_LEFT)
            return false;
        const int heading = headingAt(event.button.x, event.button.y);
        if (heading < 0)
            return false;
        toggle(headings_[static_cast<size_t>(heading)].block);
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        if (!ui::contains(bounds_, event.wheel.mouse_x, event.wheel.mouse_y))
            return false;
        const float dy = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
        scrollY_ -= dy * kWheelLines * lh;
        break;
    }
    case SDL_EVENT_KEY_DOWN: {
        const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
        switch (event.key.key) {
        case SDLK_UP:
            scrollY_ -= lh;
            break;
        case SDLK_DOWN:
            scrollY_ += lh;
            break;
        case SDLK_PAGEUP:
            scrollY_ -= page;
            break;
        case SDLK_PAGEDOWN:
            scrollY_ += page;
            break;
        case SDLK_SPACE:
            scrollY_ += shift ? -page : page;
            break;
        case SDLK_HOME:
            scrollY_ = 0.0f;
            break;
        case SDLK_END:
            scrollY_ = contentHeight_;
            break;
        default:
            return false;
        }
        break;
    }
    default:
        return false;
    }
    clampScroll();
    if (event.type == SDL_EVENT_MOUSE_WHEEL) // content moved under the pointer
        hoverHeading_ = headingAt(event.wheel.mouse_x, event.wheel.mouse_y);
    return scrollY_ != before;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void MarkdownView::draw(SDL_Renderer* renderer)
{
    if (layoutWidth_ != columnWidth())
        layout();
    clampScroll();

    ui::fillRect(renderer, bounds_, theme::kEditorBg);
    const SDL_Rect clip = ui::toPixelRect(bounds_);
    SDL_SetRenderClipRect(renderer, &clip);

    const float ox = columnLeft();
    const float sy = std::round(scrollY_);
    const float oy = bounds_.y - sy;
    const float viewBottom = sy + bounds_.h;

    for (const Box& box : boxes_) {
        if (box.rect.y + box.rect.h < sy || box.rect.y > viewBottom)
            continue;
        ui::fillRect(renderer, {ox + box.rect.x, oy + box.rect.y, box.rect.w, box.rect.h}, box.color);
    }

    // Lines are sorted by y (heights vary, e.g. between table cells): skip to
    // the first line that could reach the viewport, then cull the rest.
    auto it = std::partition_point(lines_.begin(), lines_.end(),
                                   [&](const Line& line) { return line.y + maxLineHeight_ < sy; });
    for (; it != lines_.end() && it->y <= viewBottom; ++it) {
        if (it->y + it->height < sy)
            continue;
        const float baselineY = oy + it->y + it->baseline;
        for (const Run& run : it->runs) {
            const Font& font = run.text->font();
            const float x = ox + run.x;
            if (run.image) {
                // Drawn right away; text queued so far still lands on top,
                // which only matters for overlapping runs (there are none).
                const SDL_FRect dst{x, baselineY - run.imageHeight, run.width, run.imageHeight};
                SDL_RenderTexture(renderer, run.image, nullptr, &dst);
                continue;
            }
            if (run.codeBackground) {
                const float pad = std::round(em() * 0.2f);
                ui::fillRect(renderer, {x - pad, baselineY - static_cast<float>(font.ascent()),
                                        run.width + 2.0f * pad, static_cast<float>(font.lineHeight())},
                             theme::kCodeBg);
            }
            if (run.decoration != 0) {
                const float thickness = std::max(1.0f, std::round(static_cast<float>(font.lineHeight()) / 18.0f));
                if (run.decoration & kUnderline)
                    ui::fillRect(renderer, {x, baselineY + thickness, run.width, thickness}, run.color);
                if (run.decoration & kStrikeThrough)
                    ui::fillRect(renderer, {x, std::round(baselineY - static_cast<float>(font.ascent()) * 0.3f),
                                            run.width, thickness}, run.color);
            }
            run.text->draw(x, baselineY - static_cast<float>(font.ascent()), run.str, run.color);
        }
    }
    fonts_.regular->flush(); // every renderer shares one batch

    // Disclosure triangles in the left margin: always shown on collapsed
    // headings, on hover for expanded ones.
    const float markerX = ox - std::round(em() * 0.9f);
    for (size_t i = 0; i < headings_.size(); ++i) {
        const HeadingHit& h = headings_[i];
        if (h.bottom < sy || h.top > viewBottom)
            continue;
        const bool collapsed = collapsed_.count(headingKeys_[h.block]) != 0;
        const bool hovered = static_cast<int>(i) == hoverHeading_;
        if (collapsed || hovered)
            drawMarker(renderer, markerX, oy + h.markerY, collapsed, hovered ? theme::kText : theme::kMuted);
    }

    SDL_SetRenderClipRect(renderer, nullptr);
}
