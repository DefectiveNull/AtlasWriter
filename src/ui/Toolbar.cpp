#include "ui/Toolbar.h"

#include "text/Font.h"
#include "text/TextRenderer.h"
#include "ui/Draw.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

void triangle(SDL_Renderer* renderer, SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_FColor color)
{
    const SDL_Vertex v[3] = {{a, color, {}}, {b, color, {}}, {c, color, {}}};
    SDL_RenderGeometry(renderer, nullptr, v, 3, nullptr, 0);
}

} // namespace

void drawIcon(SDL_Renderer* renderer, Toolbar::Icon icon, float cx, float cy, float size, SDL_FColor color)
{
    const float h = size * 0.5f;
    const float bar = std::max(2.0f, std::round(size * 0.16f));
    switch (icon) {
    case Toolbar::Icon::None:
        break;
    case Toolbar::Icon::Back:
        triangle(renderer, {cx + h * 0.6f, cy - h}, {cx + h * 0.6f, cy + h}, {cx - h * 0.7f, cy}, color);
        break;
    case Toolbar::Icon::Forward:
    case Toolbar::Icon::Play:
        triangle(renderer, {cx - h * 0.6f, cy - h}, {cx - h * 0.6f, cy + h}, {cx + h * 0.8f, cy}, color);
        break;
    case Toolbar::Icon::Pause: {
        const float w = std::round(size * 0.28f);
        fillRect(renderer, {std::round(cx - h * 0.75f), std::round(cy - h), w, std::round(size)}, color);
        fillRect(renderer, {std::round(cx + h * 0.75f - w), std::round(cy - h), w, std::round(size)}, color);
        break;
    }
    case Toolbar::Icon::Minus:
        fillRect(renderer, {std::round(cx - h), std::round(cy - bar * 0.5f), std::round(size), bar}, color);
        break;
    case Toolbar::Icon::Plus:
        fillRect(renderer, {std::round(cx - h), std::round(cy - bar * 0.5f), std::round(size), bar}, color);
        fillRect(renderer, {std::round(cx - bar * 0.5f), std::round(cy - h), bar, std::round(size)}, color);
        break;
    case Toolbar::Icon::Reload: {
        // A 300-degree arc with an arrowhead at its end.
        constexpr int kSteps = 20;
        const float r = h * 0.85f;
        const float start = 0.9f, sweep = 5.2f; // radians, from the top-right going round
        SDL_FPoint points[kSteps + 1];
        for (int i = 0; i <= kSteps; ++i) {
            const float a = start + sweep * static_cast<float>(i) / kSteps;
            points[i] = {cx + r * std::sin(a), cy - r * std::cos(a)};
        }
        SDL_SetRenderDrawColorFloat(renderer, color.r, color.g, color.b, color.a);
        for (float d = -0.6f; d <= 0.6f; d += 0.6f) { // a little thickness
            SDL_FPoint thick[kSteps + 1];
            for (int i = 0; i <= kSteps; ++i)
                thick[i] = {points[i].x + d, points[i].y + d};
            SDL_RenderLines(renderer, thick, kSteps + 1);
        }
        const float a = start; // the arrowhead points along the start of the arc, backwards
        const SDL_FPoint tip{cx + r * std::sin(a - 0.5f), cy - r * std::cos(a - 0.5f)};
        const SDL_FPoint base{cx + r * std::sin(a), cy - r * std::cos(a)};
        const float nx = std::sin(a), ny = -std::cos(a); // outward normal at the base
        const float w = size * 0.22f;
        triangle(renderer, tip, {base.x + nx * w, base.y + ny * w}, {base.x - nx * w, base.y - ny * w}, color);
        break;
    }
    }
}

} // namespace ui

float Toolbar::heightFor(const TextRenderer& text)
{
    return std::round(static_cast<float>(text.font().lineHeight()) * 1.7f);
}

void Toolbar::set(const SDL_FRect& bar, const TextRenderer& text, std::vector<Item> items)
{
    bar_ = bar;
    items_ = std::move(items);
    rects_.assign(items_.size(), SDL_FRect{});
    const float em = text.font().pixelSize();
    const float pad = std::round(em * 0.6f);
    const float gap = std::round(em * 0.25f);
    const float iconSize = std::round(static_cast<float>(text.font().lineHeight()) * 0.5f);
    const float h = std::round(bar.h - 2.0f * gap);
    const float y = bar.y + std::round((bar.h - h) * 0.5f);

    const auto width = [&](const Item& item) {
        float w = 0.0f;
        if (item.icon != Icon::None)
            w += iconSize;
        if (!item.text.empty())
            w += (w > 0.0f ? std::round(em * 0.4f) : 0.0f) + text.measure(item.text);
        return w + 2.0f * pad;
    };

    float left = bar.x + gap;
    float right = bar.x + bar.w - gap;
    for (size_t i = items_.size(); i-- > 0;) {
        if (!items_[i].right)
            continue;
        const float w = width(items_[i]);
        rects_[i] = {right - w, y, w, h};
        right -= w + gap;
    }
    int flex = -1;
    float fixed = 0.0f;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].right)
            continue;
        if (items_[i].flex && flex < 0)
            flex = static_cast<int>(i);
        else
            fixed += width(items_[i]) + gap;
    }
    const float flexWidth = std::max(0.0f, right - left - fixed);
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].right)
            continue;
        const float w = static_cast<int>(i) == flex ? flexWidth : width(items_[i]);
        rects_[i] = {left, y, std::min(w, std::max(0.0f, right - left)), h};
        left += w + gap;
    }
}

int Toolbar::hit(float x, float y) const
{
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id != 0 && items_[i].enabled && rects_[i].w > 0.0f && ui::contains(rects_[i], x, y))
            return items_[i].id;
    }
    return 0;
}

bool Toolbar::hover(float x, float y)
{
    const int now = hit(x, y);
    if (now == hovered_)
        return false;
    hovered_ = now;
    return true;
}

SDL_FRect Toolbar::rectOf(int id) const
{
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id == id)
            return rects_[i];
    }
    return {};
}

void Toolbar::draw(SDL_Renderer* renderer, TextRenderer& text) const
{
    ui::fillRect(renderer, bar_, theme::kPanelBg);
    ui::fillRect(renderer, {bar_.x, bar_.y + bar_.h - 1.0f, bar_.w, 1.0f}, theme::kBorder);
    const float em = text.font().pixelSize();
    const float pad = std::round(em * 0.6f);
    const float lh = static_cast<float>(text.font().lineHeight());
    const float iconSize = std::round(lh * 0.5f);

    for (size_t i = 0; i < items_.size(); ++i) {
        const Item& item = items_[i];
        const SDL_FRect& r = rects_[i];
        if (r.w <= 0.0f)
            continue;
        if (item.boxed) {
            ui::fillRect(renderer, r, theme::kEditorBg);
            ui::strokeRect(renderer, r, theme::kBorder);
        }
        if (item.id != 0 && item.enabled && item.id == hovered_)
            ui::fillRect(renderer, r, theme::kMenuHover);
        const SDL_FColor color = !item.enabled ? theme::kGutterText : item.id == 0 && !item.boxed ? theme::kMuted
                                                                                                  : theme::kMenuText;
        float x = r.x + pad;
        if (item.icon != Icon::None) {
            ui::drawIcon(renderer, item.icon, std::round(x + iconSize * 0.5f), std::round(r.y + r.h * 0.5f), iconSize,
                         color);
            x += iconSize + std::round(em * 0.4f);
        }
        if (!item.text.empty())
            text.draw(x, std::round(r.y + (r.h - lh) * 0.5f), item.text, color, r.x, r.x + r.w - pad);
    }
    text.flush();
}

void ui::drawMessage(SDL_Renderer* renderer, TextRenderer& text, const SDL_FRect& area,
                     const std::vector<std::string>& lines)
{
    const float lh = std::round(static_cast<float>(text.font().lineHeight()) * 1.4f);
    float y = std::round(area.y + (area.h - lh * static_cast<float>(lines.size())) * 0.5f);
    const float margin = std::round(text.font().pixelSize());
    const SDL_Rect clip = toPixelRect(area);
    SDL_SetRenderClipRect(renderer, &clip);
    for (size_t i = 0; i < lines.size(); ++i) {
        // Long lines (error details) start at the margin instead of centring.
        const float w = text.measure(lines[i]);
        const float x = w > area.w - 2.0f * margin ? area.x + margin : std::round(area.x + (area.w - w) * 0.5f);
        text.draw(x, y, lines[i], i == 0 ? theme::kMenuText : theme::kMuted, area.x, area.x + area.w - margin);
        y += lh;
    }
    text.flush();
    SDL_SetRenderClipRect(renderer, nullptr);
}
