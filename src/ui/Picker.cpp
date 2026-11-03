#include "Picker.h"

#include "text/Font.h"
#include "text/TextRenderer.h"
#include "text/Utf8.h"
#include "ui/Draw.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr size_t kMaxRows = 10;

bool containsIgnoreCase(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
        return true;
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
        return SDL_tolower(static_cast<unsigned char>(a)) == SDL_tolower(static_cast<unsigned char>(b));
    });
    return it != haystack.end();
}

} // namespace

Picker::Picker(TextRenderer& text)
    : text_(text)
{
}

void Picker::open(int id, std::string prompt, std::vector<std::string> choices, bool freeText)
{
    open_ = true;
    id_ = id;
    prompt_ = std::move(prompt);
    choices_ = std::move(choices);
    freeText_ = freeText;
    typed_.clear();
    done_ = false;
    filter();
}

void Picker::close()
{
    open_ = false;
}

void Picker::filter()
{
    shown_.clear();
    for (size_t i = 0; i < choices_.size(); ++i) {
        if (containsIgnoreCase(choices_[i], typed_))
            shown_.push_back(i);
    }
    selected_ = freeText_ || shown_.empty() ? -1 : 0;
}

void Picker::choose()
{
    if (selected_ >= 0 && static_cast<size_t>(selected_) < shown_.size())
        result_ = choices_[shown_[static_cast<size_t>(selected_)]];
    else if (freeText_)
        result_ = typed_;
    else
        return;
    // Trim; an empty answer is no answer.
    while (!result_.empty() && result_.back() == ' ')
        result_.pop_back();
    while (!result_.empty() && result_.front() == ' ')
        result_.erase(0, 1);
    if (result_.empty())
        return;
    done_ = true;
    open_ = false;
}

bool Picker::handleEvent(const SDL_Event& event)
{
    if (!open_)
        return false;
    switch (event.type) {
    case SDL_EVENT_TEXT_INPUT:
        typed_ += event.text.text;
        filter();
        return true;
    case SDL_EVENT_KEY_DOWN:
        switch (event.key.key) {
        case SDLK_ESCAPE:
            close();
            return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            choose();
            return true;
        case SDLK_BACKSPACE:
            if (!typed_.empty()) {
                size_t end = typed_.size() - 1;
                while (end > 0 && utf8::isContinuation(typed_[end]))
                    --end;
                typed_.erase(end);
                filter();
            }
            return true;
        case SDLK_UP:
            if (!shown_.empty())
                selected_ = selected_ <= 0 ? static_cast<int>(std::min(shown_.size(), kMaxRows)) - 1 : selected_ - 1;
            return true;
        case SDLK_DOWN:
            if (!shown_.empty())
                selected_ = (selected_ + 1) % static_cast<int>(std::min(shown_.size(), kMaxRows));
            return true;
        default:
            return false;
        }
    case SDL_EVENT_MOUSE_MOTION: {
        const SDL_FRect b = box();
        const float listTop = b.y + rowHeight();
        if (ui::contains(b, event.motion.x, event.motion.y) && event.motion.y >= listTop) {
            const auto row = static_cast<int>((event.motion.y - listTop) / rowHeight());
            if (row < static_cast<int>(std::min(shown_.size(), kMaxRows)) && row != selected_) {
                selected_ = row;
                return true;
            }
        }
        return false;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const SDL_FRect b = box();
        if (!ui::contains(b, event.button.x, event.button.y)) {
            close();
            return true;
        }
        const float listTop = b.y + rowHeight();
        if (event.button.y >= listTop) {
            const auto row = static_cast<int>((event.button.y - listTop) / rowHeight());
            if (row < static_cast<int>(std::min(shown_.size(), kMaxRows))) {
                selected_ = row;
                choose();
            }
        }
        return true;
    }
    default:
        return false;
    }
}

bool Picker::takeResult(std::string& out)
{
    if (!done_)
        return false;
    done_ = false;
    out = std::move(result_);
    return true;
}

float Picker::rowHeight() const
{
    return std::round(static_cast<float>(text_.font().lineHeight()) * 1.5f);
}

SDL_FRect Picker::box() const
{
    const float em = text_.font().pixelSize();
    const float width = std::min(windowWidth_ - 2.0f * em, std::round(em * 30.0f));
    const size_t rows = 1 + std::min(shown_.size(), kMaxRows);
    return {std::round((windowWidth_ - width) * 0.5f), top_ + std::round(em * 0.5f), width,
            rowHeight() * static_cast<float>(rows)};
}

void Picker::draw(SDL_Renderer* renderer, float windowWidth, float top)
{
    windowWidth_ = windowWidth;
    top_ = top;
    if (!open_)
        return;
    const SDL_FRect b = box();
    const float em = text_.font().pixelSize();
    const float lh = static_cast<float>(text_.font().lineHeight());
    const float rh = rowHeight();
    const float pad = std::round(em * 0.6f);
    const float textOffset = std::round((rh - lh) * 0.5f);

    ui::fillRect(renderer, {b.x + 3.0f, b.y + 3.0f, b.w, b.h}, theme::kShadow);
    ui::fillRect(renderer, b, theme::kMenuBg);
    ui::strokeRect(renderer, b, theme::kBorder);

    // The input line: the prompt, then what was typed and a caret.
    const SDL_FRect input{b.x, b.y, b.w, rh};
    ui::fillRect(renderer, input, theme::kEditorBg);
    const float promptWidth = text_.measure(prompt_);
    text_.draw(b.x + pad, b.y + textOffset, prompt_, theme::kMenuShortcut);
    const float typedX = std::round(b.x + pad + promptWidth + em * 0.5f);
    text_.draw(typedX, b.y + textOffset, typed_, theme::kMenuText, b.x, b.x + b.w - pad);
    ui::fillRect(renderer, {std::round(typedX + text_.measure(typed_)), b.y + textOffset, 1.0f, lh}, theme::kCaret);

    for (size_t row = 0; row < std::min(shown_.size(), kMaxRows); ++row) {
        const float y = b.y + rh * static_cast<float>(row + 1);
        if (static_cast<int>(row) == selected_)
            ui::fillRect(renderer, {b.x, y, b.w, rh}, theme::kMenuActive);
        text_.draw(b.x + pad, y + textOffset, choices_[shown_[row]], theme::kMenuText, b.x, b.x + b.w - pad);
    }
    text_.flush();
}
