#pragma once

#include <SDL3/SDL.h>

#include <cmath>

namespace theme {

inline constexpr SDL_FColor kEditorBg{0.118f, 0.118f, 0.133f, 1.0f};
inline constexpr SDL_FColor kGutterBg{0.102f, 0.102f, 0.114f, 1.0f};
inline constexpr SDL_FColor kGutterText{0.40f, 0.40f, 0.45f, 1.0f};
inline constexpr SDL_FColor kGutterActive{0.80f, 0.80f, 0.85f, 1.0f};
inline constexpr SDL_FColor kText{0.85f, 0.85f, 0.87f, 1.0f};
inline constexpr SDL_FColor kCurrentLine{1.0f, 1.0f, 1.0f, 0.04f};
inline constexpr SDL_FColor kSelection{0.20f, 0.42f, 0.72f, 0.45f};
inline constexpr SDL_FColor kCaret{0.92f, 0.92f, 0.96f, 1.0f};

// Rendered Markdown
inline constexpr SDL_FColor kHeading{0.95f, 0.95f, 0.97f, 1.0f};
inline constexpr SDL_FColor kMuted{0.55f, 0.55f, 0.60f, 1.0f};
inline constexpr SDL_FColor kLink{0.40f, 0.68f, 0.98f, 1.0f};
inline constexpr SDL_FColor kCodeText{0.86f, 0.74f, 0.58f, 1.0f};
inline constexpr SDL_FColor kCodeBg{1.0f, 1.0f, 1.0f, 0.05f};
inline constexpr SDL_FColor kQuoteText{0.68f, 0.68f, 0.72f, 1.0f};
inline constexpr SDL_FColor kQuoteBar{0.32f, 0.32f, 0.36f, 1.0f};
inline constexpr SDL_FColor kRule{0.26f, 0.26f, 0.29f, 1.0f};
inline constexpr SDL_FColor kTableBorder{0.26f, 0.26f, 0.29f, 1.0f};
inline constexpr SDL_FColor kTableHeaderRule{0.40f, 0.40f, 0.45f, 1.0f};
inline constexpr SDL_FColor kTableHeaderBg{1.0f, 1.0f, 1.0f, 0.06f};
inline constexpr SDL_FColor kTableStripe{1.0f, 1.0f, 1.0f, 0.025f};

// Syntax highlighting
inline constexpr SDL_FColor kSynKeyword{0.77f, 0.53f, 0.75f, 1.0f};
inline constexpr SDL_FColor kSynType{0.31f, 0.79f, 0.69f, 1.0f};
inline constexpr SDL_FColor kSynLiteral{0.34f, 0.61f, 0.84f, 1.0f};
inline constexpr SDL_FColor kSynString{0.81f, 0.57f, 0.47f, 1.0f};
inline constexpr SDL_FColor kSynNumber{0.71f, 0.81f, 0.66f, 1.0f};
inline constexpr SDL_FColor kSynComment{0.42f, 0.60f, 0.33f, 1.0f};
inline constexpr SDL_FColor kSynPreprocessor{0.61f, 0.61f, 0.86f, 1.0f};
inline constexpr SDL_FColor kSynFunction{0.86f, 0.86f, 0.67f, 1.0f};

inline constexpr SDL_FColor kPanelBg{0.102f, 0.102f, 0.114f, 1.0f};
inline constexpr SDL_FColor kTerminalBg{0.090f, 0.090f, 0.100f, 1.0f};

inline constexpr SDL_FColor kMenuBarBg{0.150f, 0.150f, 0.168f, 1.0f};
inline constexpr SDL_FColor kMenuBg{0.172f, 0.172f, 0.192f, 1.0f};
inline constexpr SDL_FColor kMenuHover{1.0f, 1.0f, 1.0f, 0.08f};
inline constexpr SDL_FColor kMenuActive{0.0f, 0.478f, 0.80f, 1.0f};
inline constexpr SDL_FColor kMenuText{0.88f, 0.88f, 0.90f, 1.0f};
inline constexpr SDL_FColor kMenuShortcut{0.55f, 0.55f, 0.60f, 1.0f};
inline constexpr SDL_FColor kBorder{0.26f, 0.26f, 0.29f, 1.0f};
inline constexpr SDL_FColor kShadow{0.0f, 0.0f, 0.0f, 0.35f};

inline constexpr SDL_FColor kStatusBg{0.0f, 0.478f, 0.80f, 1.0f};
inline constexpr SDL_FColor kStatusText{1.0f, 1.0f, 1.0f, 1.0f};

} // namespace theme

namespace ui {

inline void fillRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_FColor color)
{
    SDL_SetRenderDrawColorFloat(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &rect);
}

inline void strokeRect(SDL_Renderer* renderer, const SDL_FRect& rect, SDL_FColor color)
{
    SDL_SetRenderDrawColorFloat(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderRect(renderer, &rect);
}

inline SDL_Rect toPixelRect(const SDL_FRect& r)
{
    return {static_cast<int>(std::floor(r.x)), static_cast<int>(std::floor(r.y)),
            static_cast<int>(std::ceil(r.w)), static_cast<int>(std::ceil(r.h))};
}

inline bool contains(const SDL_FRect& rect, float x, float y)
{
    const SDL_FPoint point{x, y};
    return SDL_PointInRectFloat(&point, &rect);
}

} // namespace ui
