#include "OutlinePanel.h"

#include "editor/TextBuffer.h"
#include "markdown/Markdown.h"
#include "text/Font.h"
#include "text/TextRenderer.h"
#include "ui/Draw.h"

#include <algorithm>
#include <cmath>

std::vector<OutlineEntry> scanOutline(const TextBuffer& buffer)
{
    std::vector<OutlineEntry> entries;
    std::string scratch;
    char fence = 0; // inside a fenced code block opened with this character
    size_t fenceLength = 0;

    for (size_t line = 0; line < buffer.lineCount(); ++line) {
        std::string_view s = buffer.line(line, scratch);
        size_t indent = 0;
        while (indent < s.size() && indent < 4 && s[indent] == ' ')
            ++indent;
        if (indent > 3)
            continue;
        s.remove_prefix(indent);
        if (s.empty())
            continue;

        if (s[0] == '`' || s[0] == '~') {
            size_t n = 0;
            while (n < s.size() && s[n] == s[0])
                ++n;
            if (n >= 3) {
                if (fence == 0) {
                    fence = s[0];
                    fenceLength = n;
                } else if (s[0] == fence && n >= fenceLength) {
                    fence = 0;
                }
                continue;
            }
        }
        if (fence != 0 || s[0] != '#')
            continue;

        int level = 0;
        while (level < static_cast<int>(s.size()) && s[static_cast<size_t>(level)] == '#')
            ++level;
        if (level > 6 || (static_cast<size_t>(level) < s.size() && s[static_cast<size_t>(level)] != ' '
                          && s[static_cast<size_t>(level)] != '\t'))
            continue;
        std::string_view title = s.substr(static_cast<size_t>(level));
        while (!title.empty() && (title.front() == ' ' || title.front() == '\t'))
            title.remove_prefix(1);
        while (!title.empty() && (title.back() == ' ' || title.back() == '\t' || title.back() == '#'))
            title.remove_suffix(1);
        if (title.empty())
            continue;

        std::string text;
        for (const md::Span& span : md::parseInline(title))
            text += span.text;
        entries.push_back({level, std::move(text), line});
    }
    return entries;
}

OutlinePanel::OutlinePanel(TextRenderer& text)
    : text_(text)
{
}

void OutlinePanel::update(const TextBuffer& buffer)
{
    if (buffer.version() == version_)
        return;
    version_ = buffer.version();
    entries_ = scanOutline(buffer);
    hover_ = -1;
    clampScroll();
}

void OutlinePanel::setCurrentLine(size_t line)
{
    // The last heading at or above the line owns its section.
    const auto it = std::upper_bound(entries_.begin(), entries_.end(), line,
                                     [](size_t l, const OutlineEntry& e) { return l < e.line; });
    current_ = static_cast<int>(it - entries_.begin()) - 1;
}

float OutlinePanel::itemHeight() const
{
    return std::round(static_cast<float>(text_.font().lineHeight()) * 1.5f);
}

float OutlinePanel::headerHeight() const
{
    return std::round(static_cast<float>(text_.font().lineHeight()) * 2.0f);
}

bool OutlinePanel::contains(float x, float y) const
{
    return ui::contains(bounds_, x, y);
}

int OutlinePanel::entryAt(float x, float y) const
{
    if (!contains(x, y) || y < bounds_.y + headerHeight())
        return -1;
    const float docY = y - bounds_.y - headerHeight() + std::round(scrollY_);
    const int index = static_cast<int>(std::floor(docY / itemHeight()));
    return index >= 0 && index < static_cast<int>(entries_.size()) ? index : -1;
}

void OutlinePanel::clampScroll()
{
    const float content = static_cast<float>(entries_.size()) * itemHeight();
    scrollY_ = std::clamp(scrollY_, 0.0f, std::max(0.0f, content - (bounds_.h - headerHeight())));
}

bool OutlinePanel::handleEvent(const SDL_Event& event, long long& jumpTo)
{
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        const int entry = entryAt(event.motion.x, event.motion.y);
        if (entry == hover_)
            return false;
        hover_ = entry;
        return true;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (hover_ < 0)
            return false;
        hover_ = -1;
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (event.button.button != SDL_BUTTON_LEFT)
            return false;
        const int entry = entryAt(event.button.x, event.button.y);
        if (entry < 0)
            return false;
        jumpTo = static_cast<long long>(entries_[static_cast<size_t>(entry)].line);
        current_ = entry;
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        if (!contains(event.wheel.mouse_x, event.wheel.mouse_y))
            return false;
        const float dy = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
        const float before = scrollY_;
        scrollY_ -= dy * 3.0f * itemHeight();
        clampScroll();
        hover_ = entryAt(event.wheel.mouse_x, event.wheel.mouse_y);
        return scrollY_ != before;
    }
    default:
        return false;
    }
}

void OutlinePanel::draw(SDL_Renderer* renderer)
{
    clampScroll();
    const float lh = static_cast<float>(text_.font().lineHeight());
    const float pad = std::round(lh * 0.7f);
    const float indent = std::round(lh * 0.8f);
    const float rowH = itemHeight();
    const float header = headerHeight();

    ui::fillRect(renderer, bounds_, theme::kPanelBg);
    ui::fillRect(renderer, {bounds_.x + bounds_.w - 1.0f, bounds_.y, 1.0f, bounds_.h}, theme::kBorder);
    const SDL_Rect clip = ui::toPixelRect({bounds_.x, bounds_.y, bounds_.w - 1.0f, bounds_.h});
    SDL_SetRenderClipRect(renderer, &clip);

    text_.draw(bounds_.x + pad, bounds_.y + std::round((header - lh) * 0.5f), "OUTLINE", theme::kMuted);
    if (entries_.empty())
        text_.draw(bounds_.x + pad, bounds_.y + header, "No headings", theme::kMuted);
    text_.flush();

    const SDL_Rect listClip = ui::toPixelRect({bounds_.x, bounds_.y + header, bounds_.w - 1.0f, bounds_.h - header});
    SDL_SetRenderClipRect(renderer, &listClip);
    const float top = bounds_.y + header - std::round(scrollY_);
    const auto first = static_cast<size_t>(std::max(0.0f, std::floor(scrollY_ / rowH)));
    const size_t last = std::min(entries_.size(), first + static_cast<size_t>(bounds_.h / rowH) + 2);
    const float right = bounds_.x + bounds_.w - pad;
    for (size_t i = first; i < last; ++i) {
        const OutlineEntry& entry = entries_[i];
        const float y = top + static_cast<float>(i) * rowH;
        const bool current = static_cast<int>(i) == current_;
        if (static_cast<int>(i) == hover_)
            ui::fillRect(renderer, {bounds_.x, y, bounds_.w - 1.0f, rowH}, theme::kMenuHover);
        if (current)
            ui::fillRect(renderer, {bounds_.x, y, std::max(2.0f, std::round(lh / 8.0f)), rowH}, theme::kMenuActive);
        const float x = bounds_.x + pad + static_cast<float>(std::min(entry.level, 6) - 1) * indent;
        const SDL_FColor color = current ? theme::kHeading : (entry.level == 1 ? theme::kText : theme::kQuoteText);
        text_.draw(x, y + std::round((rowH - lh) * 0.5f), entry.text, color, bounds_.x, right);
    }
    text_.flush();
    SDL_SetRenderClipRect(renderer, nullptr);
}
