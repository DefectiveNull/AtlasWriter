#include "MenuBar.h"

#include "text/Font.h"
#include "text/TextRenderer.h"
#include "ui/Draw.h"

#include <algorithm>
#include <cmath>

MenuBar::MenuBar(const Font& font, TextRenderer& text)
    : font_(font)
    , text_(text)
{
}

void MenuBar::addMenu(Menu menu)
{
    menus_.push_back(std::move(menu));
}

void MenuBar::clear()
{
    menus_.clear();
    open_ = hoverTitle_ = hoverItem_ = -1;
}

void MenuBar::setEnabled(int command, bool enabled)
{
    for (Menu& menu : menus_) {
        for (MenuItem& item : menu.items) {
            if (item.command == command)
                item.enabled = enabled;
        }
    }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

float MenuBar::height() const
{
    return std::round(static_cast<float>(font_.lineHeight()) * 1.5f);
}

float MenuBar::padX() const
{
    return std::round(static_cast<float>(font_.lineHeight()) * 0.6f);
}

float MenuBar::padY() const
{
    return std::round(static_cast<float>(font_.lineHeight()) * 0.25f);
}

float MenuBar::itemHeight(const MenuItem& item) const
{
    const float lh = static_cast<float>(font_.lineHeight());
    return std::round(item.command == 0 ? lh * 0.6f : lh * 1.5f);
}

SDL_FRect MenuBar::titleRect(int index) const
{
    float x = 0.0f;
    float w = 0.0f;
    for (int i = 0; i <= index; ++i) {
        x += w;
        w = std::round(text_.measure(menus_[static_cast<size_t>(i)].title) + 2.0f * padX());
    }
    return {x, 0.0f, w, height()};
}

SDL_FRect MenuBar::dropdownRect() const
{
    const Menu& menu = menus_[static_cast<size_t>(open_)];
    float labelWidth = 0.0f;
    float shortcutWidth = 0.0f;
    float contentHeight = 0.0f;
    for (const MenuItem& item : menu.items) {
        labelWidth = std::max(labelWidth, text_.measure(item.label));
        shortcutWidth = std::max(shortcutWidth, text_.measure(item.shortcut));
        contentHeight += itemHeight(item);
    }

    const float lh = static_cast<float>(font_.lineHeight());
    const float gap = shortcutWidth > 0.0f ? 3.0f * padX() : 0.0f;
    const float width = std::max(labelWidth + gap + shortcutWidth + 2.0f * padX(), lh * 10.0f);
    return {titleRect(open_).x, height(), std::round(width), contentHeight + 2.0f * padY()};
}

int MenuBar::titleAt(float x, float y) const
{
    if (y < 0.0f || y >= height())
        return -1;
    for (int i = 0; i < static_cast<int>(menus_.size()); ++i) {
        if (ui::contains(titleRect(i), x, y))
            return i;
    }
    return -1;
}

int MenuBar::itemAt(float x, float y) const
{
    if (open_ < 0)
        return -1;
    const SDL_FRect rect = dropdownRect();
    if (!ui::contains(rect, x, y))
        return -1;

    const auto& items = menus_[static_cast<size_t>(open_)].items;
    float top = rect.y + padY();
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const MenuItem& item = items[static_cast<size_t>(i)];
        const float h = itemHeight(item);
        if (y >= top && y < top + h)
            return item.command != 0 && item.enabled ? i : -1;
        top += h;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void MenuBar::open(int index)
{
    open_ = index;
    hoverItem_ = -1;
}

void MenuBar::close()
{
    open_ = -1;
    hoverItem_ = -1;
}

void MenuBar::step(int direction)
{
    const auto& items = menus_[static_cast<size_t>(open_)].items;
    const int n = static_cast<int>(items.size());
    int i = hoverItem_ >= 0 ? hoverItem_ : (direction > 0 ? -1 : n);
    for (int tries = 0; tries < n; ++tries) {
        i = ((i + direction) % n + n) % n;
        if (items[static_cast<size_t>(i)].command != 0 && items[static_cast<size_t>(i)].enabled) {
            hoverItem_ = i;
            return;
        }
    }
}

bool MenuBar::handleEvent(const SDL_Event& event, int& command, bool& redraw)
{
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        const float x = event.motion.x;
        const float y = event.motion.y;
        const int title = titleAt(x, y);
        if (title != hoverTitle_) {
            hoverTitle_ = title;
            redraw = true;
        }
        if (open_ < 0)
            return false; // hovering only; the editor may be drag-selecting
        if (title >= 0 && title != open_) {
            open(title);
            redraw = true;
        }
        const int item = itemAt(x, y);
        if (item != hoverItem_) {
            hoverItem_ = item;
            redraw = true;
        }
        return true;
    }

    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const float x = event.button.x;
        const float y = event.button.y;
        const int title = titleAt(x, y);
        if (title >= 0) {
            if (open_ == title)
                close();
            else
                open(title);
            redraw = true;
            return true;
        }
        if (open_ < 0)
            return y < height();

        const int item = itemAt(x, y);
        if (item >= 0)
            command = menus_[static_cast<size_t>(open_)].items[static_cast<size_t>(item)].command;
        else if (ui::contains(dropdownRect(), x, y))
            return true; // separator or padding: stay open
        close(); // chose an item, or clicked outside (which is swallowed)
        redraw = true;
        return true;
    }

    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_TEXT_INPUT:
        return open_ >= 0;

    case SDL_EVENT_KEY_DOWN:
        return handleKey(event.key, command, redraw);

    case SDL_EVENT_WINDOW_FOCUS_LOST:
        hoverTitle_ = -1;
        close();
        redraw = true;
        return false;

    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        if (hoverTitle_ >= 0) {
            hoverTitle_ = -1;
            redraw = true;
        }
        return false;

    default:
        return false;
    }
}

bool MenuBar::handleKey(const SDL_KeyboardEvent& key, int& command, bool& redraw)
{
    if (open_ < 0) {
        if ((key.mod & SDL_KMOD_ALT) == 0 || (key.mod & SDL_KMOD_CTRL) != 0)
            return false;
        for (int i = 0; i < static_cast<int>(menus_.size()); ++i) {
            const std::string& title = menus_[static_cast<size_t>(i)].title;
            if (!title.empty() && static_cast<SDL_Keycode>(SDL_tolower(title[0])) == key.key) {
                open(i);
                step(1);
                redraw = true;
                return true;
            }
        }
        return false;
    }

    const auto& items = menus_[static_cast<size_t>(open_)].items;
    const int count = static_cast<int>(menus_.size());
    switch (key.key) {
    case SDLK_ESCAPE:
        close();
        break;
    case SDLK_UP:
        step(-1);
        break;
    case SDLK_DOWN:
        step(1);
        break;
    case SDLK_LEFT:
        open((open_ + count - 1) % count);
        step(1);
        break;
    case SDLK_RIGHT:
        open((open_ + 1) % count);
        step(1);
        break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        if (hoverItem_ >= 0) {
            command = items[static_cast<size_t>(hoverItem_)].command;
            close();
        }
        break;
    default:
        break;
    }
    redraw = true;
    return true; // swallow every key while a menu is open
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void MenuBar::draw(SDL_Renderer* renderer, float width)
{
    const float h = height();
    const float lh = static_cast<float>(font_.lineHeight());
    const float textY = std::round((h - lh) * 0.5f);

    ui::fillRect(renderer, {0.0f, 0.0f, width, h}, theme::kMenuBarBg);
    ui::fillRect(renderer, {0.0f, h - 1.0f, width, 1.0f}, theme::kBorder);

    for (int i = 0; i < static_cast<int>(menus_.size()); ++i) {
        const SDL_FRect rect = titleRect(i);
        if (i == open_)
            ui::fillRect(renderer, rect, theme::kMenuActive);
        else if (i == hoverTitle_)
            ui::fillRect(renderer, rect, theme::kMenuHover);
        text_.draw(rect.x + padX(), textY, menus_[static_cast<size_t>(i)].title, theme::kMenuText);
    }
    text_.flush();
}

void MenuBar::drawDropdown(SDL_Renderer* renderer)
{
    if (open_ < 0)
        return;

    const SDL_FRect rect = dropdownRect();
    const float lh = static_cast<float>(font_.lineHeight());
    const float pad = padX();
    const float inset = std::round(padY() * 0.5f);

    ui::fillRect(renderer, {rect.x + 3.0f, rect.y + 3.0f, rect.w, rect.h}, theme::kShadow);
    ui::fillRect(renderer, rect, theme::kMenuBg);
    ui::strokeRect(renderer, rect, theme::kBorder);

    float top = rect.y + padY();
    const auto& items = menus_[static_cast<size_t>(open_)].items;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const MenuItem& item = items[static_cast<size_t>(i)];
        const float h = itemHeight(item);
        if (item.command == 0) {
            ui::fillRect(renderer, {rect.x + inset, std::round(top + h * 0.5f), rect.w - 2.0f * inset, 1.0f},
                         theme::kBorder);
        } else {
            const bool hovered = i == hoverItem_;
            if (hovered)
                ui::fillRect(renderer, {rect.x + inset, top, rect.w - 2.0f * inset, h}, theme::kMenuActive);
            const float textY = top + std::round((h - lh) * 0.5f);
            const SDL_FColor color = item.enabled ? theme::kMenuText : theme::kMuted;
            text_.draw(rect.x + pad, textY, item.label, color);
            if (!item.shortcut.empty()) {
                const float x = rect.x + rect.w - pad - text_.measure(item.shortcut);
                text_.draw(std::round(x), textY, item.shortcut,
                           hovered ? theme::kMenuText : (item.enabled ? theme::kMenuShortcut : theme::kMuted));
            }
        }
        top += h;
    }
    text_.flush();
}
