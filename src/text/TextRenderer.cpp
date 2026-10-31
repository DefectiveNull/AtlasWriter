#include "TextRenderer.h"

#include "Font.h"
#include "GlyphAtlas.h"
#include "Utf8.h"

#include <algorithm>
#include <cmath>

TextBatch::TextBatch(SDL_Renderer* renderer, GlyphAtlas& atlas)
    : renderer_(renderer)
    , atlas_(atlas)
{
}

void TextBatch::flush()
{
    const size_t quads = vertices_.size() / 4;
    quadsLastFlush_ = quads;
    if (quads == 0)
        return;

    // New glyphs only exist in the CPU copy until now: upload them in one go.
    atlas_.upload();

    while (indices_.size() < quads * 6) {
        const int base = static_cast<int>(indices_.size() / 6) * 4;
        indices_.insert(indices_.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }

    SDL_RenderGeometry(renderer_, atlas_.texture(),
                       vertices_.data(), static_cast<int>(vertices_.size()),
                       indices_.data(), static_cast<int>(quads * 6));
    vertices_.clear();
}

TextRenderer::TextRenderer(TextBatch& batch, GlyphAtlas& atlas, const Font& font)
    : batch_(batch)
    , atlas_(atlas)
    , font_(font)
    , face_(atlas.addFace(font))
{
}

// Calls fn(byteOffset, x, advance, codepoint) for every codepoint in the
// line, where x is relative to the line start and already includes the
// kerning against the previous codepoint. fn returns false to stop early.
// Uses cached advances only, so layout never touches the atlas.
template <typename Fn>
void TextRenderer::walk(std::string_view utf8, Fn&& fn) const
{
    const char* p = utf8.data();
    const char* const end = p + utf8.size();
    float x = 0.0f;
    uint32_t prev = 0; // 0: no kerning pair (line start, after a tab)

    while (p < end) {
        const size_t offset = static_cast<size_t>(p - utf8.data());
        const uint32_t codepoint = utf8::decode(p, end);

        float adv = 0.0f;
        if (codepoint == '\t') {
            const float tab = font_.advanceOf(' ') * static_cast<float>(tabSize_);
            adv = (std::floor(x / tab) + 1.0f) * tab - x;
            prev = 0;
        } else {
            if (prev != 0)
                x += font_.kern(prev, codepoint);
            adv = font_.advanceOf(codepoint);
            prev = codepoint;
        }

        if (!fn(offset, x, adv, codepoint))
            return;
        x += adv;
    }
}

void TextRenderer::draw(float x, float y, std::string_view utf8, SDL_FColor color,
                        float clipLeft, float clipRight)
{
    draw(x, y, utf8, color, nullptr, 0, clipLeft, clipRight);
}

void TextRenderer::draw(float x, float y, std::string_view utf8, SDL_FColor defaultColor, const ColorRun* runs,
                        size_t runCount, float clipLeft, float clipRight)
{
    const float invSize = 1.0f / static_cast<float>(atlas_.size());
    const float baseline = y + static_cast<float>(font_.ascent());
    std::vector<SDL_Vertex>& vertices = batch_.vertices_;
    SDL_FColor color = defaultColor;
    size_t nextRun = 0;

    walk(utf8, [&](size_t offset, float gx, float adv, uint32_t codepoint) {
        while (nextRun < runCount && runs[nextRun].start <= offset)
            color = runs[nextRun++].color;
        const float pen = x + gx;
        if (pen > clipRight)
            return false;
        if (codepoint == '\t' || codepoint == ' ' || pen + adv < clipLeft)
            return true;
        const Glyph* g = atlas_.get(face_, codepoint);
        if (!g || g->w == 0)
            return true;

        // Snap to whole pixels so glyphs stay crisp with nearest sampling.
        const float x0 = std::round(pen) + g->xoff;
        const float y0 = baseline + g->yoff;
        const float x1 = x0 + g->w;
        const float y1 = y0 + g->h;
        const float u0 = g->x * invSize;
        const float v0 = g->y * invSize;
        const float u1 = (g->x + g->w) * invSize;
        const float v1 = (g->y + g->h) * invSize;

        vertices.push_back({{x0, y0}, color, {u0, v0}});
        vertices.push_back({{x1, y0}, color, {u1, v0}});
        vertices.push_back({{x1, y1}, color, {u1, v1}});
        vertices.push_back({{x0, y1}, color, {u0, v1}});
        return true;
    });
}

float TextRenderer::advance(uint32_t codepoint) const
{
    return font_.advanceOf(codepoint);
}

float TextRenderer::measure(std::string_view utf8, size_t bytes) const
{
    bytes = std::min(bytes, utf8.size());
    float width = 0.0f;
    walk(utf8, [&](size_t offset, float x, float adv, uint32_t) {
        if (offset >= bytes) {
            width = x;
            return false;
        }
        width = x + adv;
        return true;
    });
    return width;
}

size_t TextRenderer::hitTest(std::string_view utf8, float x) const
{
    size_t result = utf8.size();
    walk(utf8, [&](size_t offset, float gx, float adv, uint32_t) {
        if (x < gx + adv * 0.5f) {
            result = offset;
            return false;
        }
        return true;
    });
    return result;
}

size_t TextRenderer::fit(std::string_view utf8, float width) const
{
    size_t result = utf8.size();
    size_t wordBreak = 0; // offset just past the last whitespace run
    bool afterSpace = false;
    walk(utf8, [&](size_t offset, float x, float adv, uint32_t codepoint) {
        const bool space = codepoint == ' ' || codepoint == '\t';
        if (space) {
            afterSpace = true;
            return true; // whitespace never forces a break
        }
        if (afterSpace) {
            wordBreak = offset;
            afterSpace = false;
        }
        if (offset > 0 && x + adv > width) {
            result = wordBreak > 0 ? wordBreak : offset;
            return false;
        }
        return true;
    });
    return result;
}
