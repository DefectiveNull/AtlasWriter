#include "PopupMenu.h"

#include "text/Font.h"
#include "text/TextRenderer.h"
#include "ui/Draw.h"

#include <algorithm>
#include <cmath>

PopupMenu::PopupMenu(TextRenderer& text)
    : text_(text)
{
}

void PopupMenu::open(const SDL_FRect& anchor, std::vector<MenuItem> items, int checkedCommand, bool keyboard)
{
    anchor_ = anchor;
    items_ = std::move(items);
    checked_ = checkedCommand;
    hover_ = -1;
    open_ = true;
    if (keyboard) {
        for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
            if (items_[static_cast<size_t>(i)].command == checkedCommand)
                hover_ = i;
        }
        if (hover_ < 0)
            step(1);
    }
}

void PopupMenu::close()
{
    open_ = false;
    hover_ = -1;
    items_.clear();
}

float PopupMenu::padX() const
{
    return std::round(static_cast<float>(text_.font().lineHeight()) * 0.6f);
}

float PopupMenu::itemHeight(const MenuItem& item) const
{
    const float lh = static_cast<float>(text_.font().lineHeight());
    return std::round(item.command == 0 ? lh * 0.6f : lh * 1.5f);
}

SDL_FRect PopupMenu::rect() const
{
    const float lh = static_cast<float>(text_.font().lineHeight());
    const float padY = std::round(lh * 0.25f);
    float labelWidth = 0.0f, shortcutWidth = 0.0f, height = 2.0f * padY;
    for (const MenuItem& item : items_) {
        labelWidth = std::max(labelWidth, text_.measure(item.label));
        shortcutWidth = std::max(shortcutWidth, text_.measure(item.shortcut));
        height += itemHeight(item);
    }
    const float gap = shortcutWidth > 0.0f ? 3.0f * padX() : 0.0f;
    // Room on the left for the check mark.
    const float width = std::round(std::max(labelWidth + gap + shortcutWidth + 3.5f * padX(), anchor_.w));
    float x = anchor_.x;
    float y = anchor_.y + anchor_.h;
    if (windowWidth_ > 0.0f)
        x = std::max(0.0f, std::min(x, windowWidth_ - width));
    if (windowHeight_ > 0.0f && y + height > windowHeight_)
        y = std::max(0.0f, anchor_.y - height); // no room below: open upward
    return {x, y, width, height};
}

int PopupMenu::itemAt(float x, float y) const
{
    const SDL_FRect r = rect();
    if (!ui::contains(r, x, y))
        return -1;
    float top = r.y + std::round(static_cast<float>(text_.font().lineHeight()) * 0.25f);
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const MenuItem& item = items_[static_cast<size_t>(i)];
        const float h = itemHeight(item);
        if (y >= top && y < top + h)
            return item.command != 0 && item.enabled ? i : -1;
        top += h;
    }
    return -1;
}

void PopupMenu::step(int direction)
{
    const int n = static_cast<int>(items_.size());
    int i = hover_ >= 0 ? hover_ : (direction > 0 ? -1 : n);
    for (int tries = 0; tries < n; ++tries) {
        i = ((i + direction) % n + n) % n;
        const MenuItem& item = items_[static_cast<size_t>(i)];
        if (item.command != 0 && item.enabled) {
            hover_ = i;
            return;
        }
    }
}

bool PopupMenu::handleEvent(const SDL_Event& event, int& command)
{
    if (!open_)
        return false;
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        const int item = itemAt(event.motion.x, event.motion.y);
        if (item == hover_ || (item < 0 && !ui::contains(rect(), event.motion.x, event.motion.y)))
            return false;
        hover_ = item;
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const int item = itemAt(event.button.x, event.button.y);
        if (item >= 0)
            command = items_[static_cast<size_t>(item)].command;
        else if (ui::contains(rect(), event.button.x, event.button.y))
            return false; // separator or padding: stay open
        close();
        return true;
    }
    case SDL_EVENT_KEY_DOWN:
        switch (event.key.key) {
        case SDLK_ESCAPE:
            close();
            break;
        case SDLK_UP:
            step(-1);
            break;
        case SDLK_DOWN:
            step(1);
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (hover_ >= 0) {
                command = items_[static_cast<size_t>(hover_)].command;
                close();
            }
            break;
        default:
            break;
        }
        return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        close();
        return true;
    default:
        return false;
    }
}

void PopupMenu::draw(SDL_Renderer* renderer, float windowWidth, float windowHeight)
{
    windowWidth_ = windowWidth;
    windowHeight_ = windowHeight;
    if (!open_)
        return;
    const SDL_FRect r = rect();
    const float lh = static_cast<float>(text_.font().lineHeight());
    const float pad = padX();
    const float padY = std::round(lh * 0.25f);
    const float inset = std::round(padY * 0.5f);

    ui::fillRect(renderer, {r.x + 3.0f, r.y + 3.0f, r.w, r.h}, theme::kShadow);
    ui::fillRect(renderer, r, theme::kMenuBg);
    ui::strokeRect(renderer, r, theme::kBorder);

    float top = r.y + padY;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const MenuItem& item = items_[static_cast<size_t>(i)];
        const float h = itemHeight(item);
        if (item.command == 0) {
            ui::fillRect(renderer, {r.x + inset, std::round(top + h * 0.5f), r.w - 2.0f * inset, 1.0f}, theme::kBorder);
        } else {
            const bool hovered = i == hover_;
            if (hovered)
                ui::fillRect(renderer, {r.x + inset, top, r.w - 2.0f * inset, h}, theme::kMenuActive);
            const float textY = top + std::round((h - lh) * 0.5f);
            if (item.command == checked_)
                text_.draw(r.x + pad, textY, "\xE2\x80\xA2", theme::kMenuText); // bullet
            const SDL_FColor color = item.enabled ? theme::kMenuText : theme::kMuted;
            text_.draw(r.x + 2.5f * pad, textY, item.label, color);
            if (!item.shortcut.empty()) {
                const float x = r.x + r.w - pad - text_.measure(item.shortcut);
                text_.draw(std::round(x), textY, item.shortcut,
                           hovered ? theme::kMenuText : (item.enabled ? theme::kMenuShortcut : theme::kMuted));
            }
        }
        top += h;
    }
    text_.flush();
}
