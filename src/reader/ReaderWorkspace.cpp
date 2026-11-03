#include "reader/ReaderWorkspace.h"

#include "embed/Embed.h"
#include "embed/SessionAssets.h"
#include "markdown/Html.h"
#include "markdown/MarkdownView.h"
#include "reader/Epub.h"
#include "reader/Pdf.h"
#include "text/Font.h"
#include "text/TextRenderer.h"
#include "ui/Draw.h"
#include "ui/ImageCache.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace {

enum ToolId : int {
    kToolZoomOut = 1,
    kToolZoomIn,
    kToolFit,
    kToolPrevChapter,
    kToolNextChapter,
};

constexpr float kWheelPixels = 48.0f; // per wheel notch, before the display scale
constexpr float kZoomSteps[] = {0.25f, 0.33f, 0.5f, 0.67f, 0.75f, 0.8f, 0.9f, 1.0f, 1.1f, 1.25f,
                                1.5f,  1.75f, 2.0f, 2.5f,  3.0f,  4.0f, 5.0f};

std::string lowerExtension(std::string_view path)
{
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
        return {};
    std::string ext(path.substr(dot + 1));
    for (char& c : ext)
        c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
    return ext;
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = std::min(text.find('\n', start), text.size());
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

bool samePathLoose(std::string a, std::string b)
{
    a = epub::normalizePath(std::move(a));
    b = epub::normalizePath(std::move(b));
#if defined(SDL_PLATFORM_WINDOWS)
    return a.size() == b.size() && SDL_strncasecmp(a.c_str(), b.c_str(), a.size()) == 0;
#else
    return a == b;
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// One open file
// ---------------------------------------------------------------------------

class ReaderWorkspace::View
{
public:
    View(const WorkspaceContext& ctx, std::string path)
        : ctx_(ctx), path_(std::move(path))
    {
    }
    virtual ~View() = default;

    const std::string& path() const { return path_; }
    virtual std::string title() const { return embed::fileName(path_); }
    virtual std::string status() = 0;
    virtual std::vector<Toolbar::Item> tools() = 0;
    virtual void tool(int) {}

    virtual void setBounds(const SDL_FRect& bounds) { bounds_ = bounds; }
    virtual bool handleEvent(const SDL_Event& event) = 0;
    virtual void draw(SDL_Renderer* renderer) = 0;
    virtual bool overLink(float, float) { return false; }
    virtual bool onAsync() { return false; }
    virtual void styleChanged() {}
    virtual void imagesLoaded() {}
    virtual bool takeOpenRequest(std::string&) { return false; }

protected:
    const WorkspaceContext& ctx_;
    std::string path_;
    SDL_FRect bounds_{};
};

namespace {

// ---------------------------------------------------------------------------
// PDF: every page in one column, rendered on PDFium's thread
// ---------------------------------------------------------------------------

class PdfView final : public ReaderWorkspace::View
{
public:
    PdfView(const WorkspaceContext& ctx, std::string path)
        : View(ctx, std::move(path)), doc_(std::make_unique<pdf::Document>(path_, ctx.asyncEvent))
    {
    }

    ~PdfView() override
    {
        for (Page& page : pages_)
            SDL_DestroyTexture(page.texture);
    }

    std::string status() override
    {
        if (doc_->status() != pdf::Document::Status::Ready)
            return "PDF";
        char text[96];
        SDL_snprintf(text, sizeof(text), "PDF \xC2\xB7 page %d of %zu \xC2\xB7 %d%%", currentPage() + 1, pages_.size(),
                     static_cast<int>(std::lround(zoom() * 100.0f)));
        return text;
    }

    std::vector<Toolbar::Item> tools() override
    {
        std::vector<Toolbar::Item> items;
        const bool ready = doc_->status() == pdf::Document::Status::Ready;
        if (ready) {
            items.push_back({0, "Page " + std::to_string(currentPage() + 1) + " / " + std::to_string(pages_.size())});
        }
        items.push_back({kToolZoomOut, "", Toolbar::Icon::Minus, ready, true});
        items.push_back({0, std::to_string(static_cast<int>(std::lround(zoom() * 100.0f))) + "%", Toolbar::Icon::None,
                         true, true});
        items.push_back({kToolZoomIn, "", Toolbar::Icon::Plus, ready, true});
        items.push_back({kToolFit, fit_ ? "Fit width \xE2\x9C\x93" : "Fit width", Toolbar::Icon::None, ready, true});
        return items;
    }

    void tool(int id) override
    {
        if (id == kToolZoomIn)
            stepZoom(1);
        else if (id == kToolZoomOut)
            stepZoom(-1);
        else if (id == kToolFit)
            setZoom(0.0f, true);
    }

    bool handleEvent(const SDL_Event& event) override
    {
        sync();
        const float before = scrollY_, beforeX = scrollX_;
        const float line = std::round(kWheelPixels * ctx_.displayScale);
        switch (event.type) {
        case SDL_EVENT_MOUSE_WHEEL: {
            if (!ui::contains(bounds_, event.wheel.mouse_x, event.wheel.mouse_y))
                return false;
            const float flip = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
            if (SDL_GetModState() & SDL_KMOD_CTRL) {
                if (event.wheel.y != 0.0f)
                    stepZoom(event.wheel.y * flip > 0.0f ? 1 : -1, event.wheel.mouse_y);
                return true;
            }
            const bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
            const float dy = event.wheel.y * flip, dx = event.wheel.x * flip;
            if (shift)
                scrollX_ -= dy * line;
            else
                scrollY_ -= dy * line;
            scrollX_ += dx * line;
            break;
        }
        case SDL_EVENT_KEY_DOWN: {
            const float page = std::max(line, bounds_.h - line);
            switch (event.key.key) {
            case SDLK_UP: scrollY_ -= line; break;
            case SDLK_DOWN: scrollY_ += line; break;
            case SDLK_LEFT: scrollX_ -= line; break;
            case SDLK_RIGHT: scrollX_ += line; break;
            case SDLK_PAGEUP: scrollY_ -= page; break;
            case SDLK_PAGEDOWN: scrollY_ += page; break;
            case SDLK_SPACE: scrollY_ += (event.key.mod & SDL_KMOD_SHIFT) ? -page : page; break;
            case SDLK_HOME: scrollY_ = 0.0f; break;
            case SDLK_END: scrollY_ = contentHeight_; break;
            case SDLK_EQUALS:
            case SDLK_PLUS:
            case SDLK_KP_PLUS: stepZoom(1); return true;
            case SDLK_MINUS:
            case SDLK_KP_MINUS: stepZoom(-1); return true;
            case SDLK_0:
            case SDLK_KP_0: setZoom(0.0f, true); return true;
            default: return false;
            }
            break;
        }
        default:
            return false;
        }
        clampScroll();
        return scrollY_ != before || scrollX_ != beforeX;
    }

    void draw(SDL_Renderer* renderer) override
    {
        sync();
        ui::fillRect(renderer, bounds_, theme::kTerminalBg);
        const pdf::Document::Status status = doc_->status();
        if (status == pdf::Document::Status::Loading) {
            ui::drawMessage(renderer, *ctx_.code, bounds_, {"Opening " + title() + "..."});
            return;
        }
        if (status == pdf::Document::Status::Failed) {
            std::vector<std::string> lines = {"Could not show " + title()};
            for (std::string& l : splitLines(doc_->error()))
                lines.push_back(std::move(l));
            ui::drawMessage(renderer, *ctx_.code, bounds_, lines);
            return;
        }
        layout();
        clampScroll();

        const SDL_Rect clip = ui::toPixelRect(bounds_);
        SDL_SetRenderClipRect(renderer, &clip);
        const float top = scrollY_, bottom = scrollY_ + bounds_.h;
        int first = -1, last = -1;
        for (size_t i = 0; i < pages_.size(); ++i) {
            const Page& p = pages_[i];
            if (p.y + p.h < top || p.y > bottom)
                continue;
            if (first < 0)
                first = static_cast<int>(i);
            last = static_cast<int>(i);
            const SDL_FRect r{std::round(bounds_.x + p.x - scrollX_), std::round(bounds_.y + p.y - scrollY_),
                              std::round(p.w), std::round(p.h)};
            ui::fillRect(renderer, {r.x + 2.0f, r.y + 3.0f, r.w, r.h}, theme::kShadow);
            if (p.texture)
                SDL_RenderTexture(renderer, p.texture, nullptr, &r);
            else
                ui::fillRect(renderer, r, {0.93f, 0.93f, 0.93f, 1.0f});
        }
        SDL_SetRenderClipRect(renderer, nullptr);
        if (first >= 0)
            request(first, last);
    }

    bool onAsync() override
    {
        bool changed = false;
        if (!opened_ && doc_->status() != pdf::Document::Status::Loading) {
            opened_ = true;
            changed = true;
        }
        for (pdf::Bitmap& bitmap : doc_->takeRendered()) {
            if (bitmap.page < 0 || static_cast<size_t>(bitmap.page) >= pages_.size() || bitmap.width <= 0)
                continue;
            Page& page = pages_[static_cast<size_t>(bitmap.page)];
            SDL_Texture* texture = SDL_CreateTexture(ctx_.renderer, SDL_PIXELFORMAT_BGRA32, SDL_TEXTUREACCESS_STATIC,
                                                     bitmap.width, bitmap.height);
            if (!texture)
                continue;
            SDL_UpdateTexture(texture, nullptr, bitmap.pixels.data(), bitmap.width * 4);
            SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
            SDL_DestroyTexture(page.texture);
            page.texture = texture;
            page.textureWidth = bitmap.width;
            changed = true;
        }
        return changed;
    }

private:
    struct Page
    {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f; // in the scrolled column
        SDL_Texture* texture = nullptr;
        int textureWidth = 0;
        int requested = 0; // width last asked for
    };

    // Copies page sizes once the document has opened.
    void sync()
    {
        if (pages_.empty() && doc_->status() == pdf::Document::Status::Ready)
            pages_.resize(doc_->pages().size());
    }

    float gap() const { return std::round(12.0f * ctx_.displayScale); }

    // Screen pixels per PDF point at 100%: 96 dpi, times the display scale.
    float basePixelsPerPoint() const { return 96.0f / 72.0f * ctx_.displayScale; }

    float zoom() const
    {
        if (!fit_)
            return zoom_;
        const std::vector<pdf::PageSize>& sizes = doc_->pages();
        float widest = 0.0f;
        for (const pdf::PageSize& s : sizes)
            widest = std::max(widest, s.width);
        if (widest <= 0.0f)
            return 1.0f;
        return std::max(0.05f, (bounds_.w - 2.0f * gap()) / (widest * basePixelsPerPoint()));
    }

    void layout()
    {
        const float z = zoom();
        if (z == laidOutZoom_ && bounds_.w == laidOutWidth_)
            return;
        // Keep the same spot of the same page at the top of the view.
        const int anchor = currentPage();
        const float within = pages_.empty() || pages_[static_cast<size_t>(anchor)].h <= 0.0f
                                 ? 0.0f
                                 : (scrollY_ - pages_[static_cast<size_t>(anchor)].y)
                                       / pages_[static_cast<size_t>(anchor)].h;
        const bool first = laidOutZoom_ < 0.0f;

        const std::vector<pdf::PageSize>& sizes = doc_->pages();
        const float scale = z * basePixelsPerPoint();
        float y = gap(), widest = 0.0f;
        for (size_t i = 0; i < pages_.size(); ++i) {
            Page& p = pages_[i];
            p.w = std::round(sizes[i].width * scale);
            p.h = std::round(sizes[i].height * scale);
            p.y = y;
            y += p.h + gap();
            widest = std::max(widest, p.w);
        }
        contentWidth_ = widest + 2.0f * gap();
        for (Page& p : pages_)
            p.x = std::round(std::max(gap(), (std::max(contentWidth_, bounds_.w) - p.w) * 0.5f));
        contentHeight_ = y;
        laidOutZoom_ = z;
        laidOutWidth_ = bounds_.w;
        if (!first && !pages_.empty())
            scrollY_ = pages_[static_cast<size_t>(anchor)].y + within * pages_[static_cast<size_t>(anchor)].h;
        clampScroll();
    }

    void clampScroll()
    {
        scrollY_ = std::clamp(scrollY_, 0.0f, std::max(0.0f, contentHeight_ - bounds_.h));
        scrollX_ = std::clamp(scrollX_, 0.0f, std::max(0.0f, contentWidth_ - bounds_.w));
    }

    int currentPage() const
    {
        // The page covering the middle of the view.
        const float mid = scrollY_ + bounds_.h * 0.5f;
        for (size_t i = 0; i < pages_.size(); ++i) {
            if (mid < pages_[i].y + pages_[i].h + gap())
                return static_cast<int>(i);
        }
        return pages_.empty() ? 0 : static_cast<int>(pages_.size() - 1);
    }

    void setZoom(float zoom, bool fit, float anchorY = -1.0f)
    {
        (void)anchorY;
        fit_ = fit;
        if (!fit)
            zoom_ = std::clamp(zoom, kZoomSteps[0], kZoomSteps[std::size(kZoomSteps) - 1]);
    }

    void stepZoom(int direction, float anchorY = -1.0f)
    {
        const float now = zoom();
        float next = now;
        if (direction > 0) {
            next = kZoomSteps[std::size(kZoomSteps) - 1];
            for (float s : kZoomSteps) {
                if (s > now + 0.001f) {
                    next = s;
                    break;
                }
            }
        } else {
            next = kZoomSteps[0];
            for (size_t i = std::size(kZoomSteps); i-- > 0;) {
                if (kZoomSteps[i] < now - 0.001f) {
                    next = kZoomSteps[i];
                    break;
                }
            }
        }
        setZoom(next, false, anchorY);
    }

    // Asks for the visible pages (and one either side) at their on-screen
    // width; drops textures far from the view.
    void request(int first, int last)
    {
        const int from = std::max(0, first - 1);
        const int to = std::min(static_cast<int>(pages_.size()) - 1, last + 1);
        std::vector<std::pair<int, int>> wanted;
        bool changed = false;
        const auto want = [&](int i) {
            Page& p = pages_[static_cast<size_t>(i)];
            const int width = std::clamp(static_cast<int>(p.w), 1, 8192);
            if (p.textureWidth == width)
                return;
            wanted.emplace_back(i, width);
            if (p.requested != width) {
                p.requested = width;
                changed = true;
            }
        };
        for (int i = first; i <= last; ++i)
            want(i);
        for (int i = from; i <= to; ++i) {
            if (i < first || i > last)
                want(i);
        }
        if (changed)
            doc_->want(std::move(wanted));

        for (int i = 0; i < static_cast<int>(pages_.size()); ++i) {
            if (i >= from - 2 && i <= to + 2)
                continue;
            Page& p = pages_[static_cast<size_t>(i)];
            if (p.texture) {
                SDL_DestroyTexture(p.texture);
                p.texture = nullptr;
                p.textureWidth = 0;
            }
            p.requested = 0;
        }
    }

    std::unique_ptr<pdf::Document> doc_;
    std::vector<Page> pages_;
    bool opened_ = false;
    bool fit_ = true;
    float zoom_ = 1.0f;
    float laidOutZoom_ = -1.0f;
    float laidOutWidth_ = -1.0f;
    float contentHeight_ = 0.0f;
    float contentWidth_ = 0.0f;
    float scrollY_ = 0.0f;
    float scrollX_ = 0.0f;
};

// ---------------------------------------------------------------------------
// EPUB: one chapter at a time, as rendered HTML
// ---------------------------------------------------------------------------

class EpubView final : public ReaderWorkspace::View
{
public:
    EpubView(const WorkspaceContext& ctx, std::string path)
        : View(ctx, std::move(path))
    {
        static int books = 0;
        folder_ = session::folderNamed("book-" + std::to_string(++books));
        if (!epub::load(path_, folder_, book_, error_))
            return;
        view_.setFonts(ctx_.markdown);
        showChapter(0, false);
    }

    ~EpubView() override { session::remove(folder_); }

    std::string title() const override { return book_.title.empty() ? embed::fileName(path_) : book_.title; }

    std::string status() override
    {
        if (!error_.empty())
            return "EPUB";
        return "EPUB \xC2\xB7 chapter " + std::to_string(chapter_ + 1) + " of " + std::to_string(book_.chapters.size());
    }

    std::vector<Toolbar::Item> tools() override
    {
        if (!error_.empty())
            return {};
        std::string label = std::to_string(chapter_ + 1) + " / " + std::to_string(book_.chapters.size());
        if (!chapterTitle_.empty())
            label += "  \xC2\xB7  " + chapterTitle_;
        return {
            {kToolPrevChapter, "", Toolbar::Icon::Back, chapter_ > 0},
            {kToolNextChapter, "", Toolbar::Icon::Forward, chapter_ + 1 < book_.chapters.size()},
            {0, label, Toolbar::Icon::None, true, false, true},
        };
    }

    void tool(int id) override
    {
        if (id == kToolPrevChapter && chapter_ > 0)
            showChapter(chapter_ - 1, false);
        else if (id == kToolNextChapter && chapter_ + 1 < book_.chapters.size())
            showChapter(chapter_ + 1, false);
    }

    void setBounds(const SDL_FRect& bounds) override
    {
        bounds_ = bounds;
        view_.setBounds(bounds);
    }

    bool handleEvent(const SDL_Event& event) override
    {
        if (!error_.empty())
            return false;
        // Paging past either end of a chapter turns to the next or previous one.
        if (event.type == SDL_EVENT_KEY_DOWN) {
            const SDL_Keycode key = event.key.key;
            const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
            const bool forward = key == SDLK_PAGEDOWN || (key == SDLK_SPACE && !shift) || key == SDLK_DOWN;
            const bool backward = key == SDLK_PAGEUP || (key == SDLK_SPACE && shift) || key == SDLK_UP;
            if (forward && view_.atBottom() && chapter_ + 1 < book_.chapters.size()) {
                showChapter(chapter_ + 1, false);
                return true;
            }
            if (backward && view_.atTop() && chapter_ > 0) {
                showChapter(chapter_ - 1, true);
                return true;
            }
            if (key == SDLK_RIGHT && chapter_ + 1 < book_.chapters.size()) {
                showChapter(chapter_ + 1, false);
                return true;
            }
            if (key == SDLK_LEFT && chapter_ > 0) {
                showChapter(chapter_ - 1, false);
                return true;
            }
        }
        return view_.handleEvent(event);
    }

    void draw(SDL_Renderer* renderer) override
    {
        if (!error_.empty()) {
            ui::fillRect(renderer, bounds_, theme::kEditorBg);
            std::vector<std::string> lines = {"Could not show " + title()};
            for (std::string& l : splitLines(error_))
                lines.push_back(std::move(l));
            ui::drawMessage(renderer, *ctx_.code, bounds_, lines);
            return;
        }
        view_.draw(renderer);
    }

    bool overLink(float x, float y) override { return error_.empty() && view_.isOverClickable(x, y); }

    void styleChanged() override
    {
        view_.setFonts(ctx_.markdown);
        view_.setImages(ctx_.images, chapterDir_, ctx_.displayScale);
        view_.invalidateLayout();
    }

    void imagesLoaded() override { view_.invalidateLayout(); }

    // Links to another chapter turn the page; anything else is the App's.
    bool takeOpenRequest(std::string& target) override
    {
        std::string url;
        if (!view_.takeLinkClick(url))
            return false;
        const std::string resolved = view_.resolveLink(url);
        if (resolved.empty())
            return false;
        for (size_t i = 0; i < book_.chapters.size(); ++i) {
            if (samePathLoose(book_.chapters[i], resolved)) {
                showChapter(i, false);
                return false;
            }
        }
        target = resolved;
        return true;
    }

private:
    void showChapter(size_t index, bool atEnd)
    {
        chapter_ = index;
        const std::string& file = book_.chapters[index];
        std::string html;
        size_t size = 0;
        if (void* data = SDL_LoadFile(file.c_str(), &size)) {
            html.assign(static_cast<const char*>(data), size);
            SDL_free(data);
        } else {
            html = "<p>This chapter is missing from the book.</p>";
        }
        chapterTitle_ = md::htmlTitle(html);
        if (chapterTitle_ == book_.title)
            chapterTitle_.clear(); // many books repeat the book's title in every chapter
        const size_t slash = file.find_last_of("/\\");
        chapterDir_ = file.substr(0, slash + 1);
        view_.setImages(ctx_.images, chapterDir_, ctx_.displayScale);
        view_.expandAll();
        view_.setSource(html, ++version_, true);
        if (atEnd)
            view_.scrollToBottom();
        else
            view_.scrollToTop();
    }

    std::string folder_;
    epub::Book book_;
    std::string error_;
    MarkdownView view_;
    size_t chapter_ = 0;
    std::string chapterTitle_;
    std::string chapterDir_;
    uint64_t version_ = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// The workspace
// ---------------------------------------------------------------------------

ReaderWorkspace::ReaderWorkspace(const WorkspaceContext& context)
    : Workspace(context)
{
}

ReaderWorkspace::~ReaderWorkspace() = default;

bool ReaderWorkspace::handles(std::string_view path)
{
    const std::string ext = lowerExtension(path);
    return ext == "pdf" || ext == "epub";
}

bool ReaderWorkspace::openTarget(const std::string& path)
{
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (samePathLoose(tabs_[i]->path(), path)) {
            selectTab(i);
            return true;
        }
    }
    std::unique_ptr<View> view;
    if (lowerExtension(path) == "pdf")
        view = std::make_unique<PdfView>(ctx_, path);
    else
        view = std::make_unique<EpubView>(ctx_, path);
    const size_t at = tabs_.empty() ? 0 : active_ + 1;
    tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(at), std::move(view));
    active_ = at;
    layout();
    return true;
}

void ReaderWorkspace::setBounds(const SDL_FRect& bounds)
{
    bounds_ = bounds;
    layout();
}

void ReaderWorkspace::layout()
{
    const float bar = Toolbar::heightFor(*ctx_.code);
    std::vector<Toolbar::Item> items;
    if (!tabs_.empty())
        items = tabs_[active_]->tools();
    toolbar_.set({bounds_.x, bounds_.y, bounds_.w, bar}, *ctx_.code, std::move(items));
    if (!tabs_.empty())
        tabs_[active_]->setBounds({bounds_.x, bounds_.y + bar, bounds_.w, std::max(0.0f, bounds_.h - bar)});
}

bool ReaderWorkspace::handleEvent(const SDL_Event& event)
{
    if (tabs_.empty())
        return false;
    layout();
    View& view = *tabs_[active_];
    if (event.type == SDL_EVENT_MOUSE_MOTION && toolbar_.hover(event.motion.x, event.motion.y))
        return true;
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        if (const int id = toolbar_.hit(event.button.x, event.button.y)) {
            view.tool(id);
            layout();
            return true;
        }
        if (ui::contains(toolbar_.bounds(), event.button.x, event.button.y))
            return false;
    }
    const bool redraw = view.handleEvent(event);
    if (redraw)
        layout(); // the page number or zoom may have changed
    return redraw;
}

void ReaderWorkspace::draw(SDL_Renderer* renderer)
{
    if (tabs_.empty()) {
        ui::fillRect(renderer, bounds_, theme::kEditorBg);
        ui::drawMessage(renderer, *ctx_.code, bounds_,
                        {"No PDF or EPUB open", "Open one with File > Open, click a link to one,",
                         "or drop it here."});
        return;
    }
    View& view = *tabs_[active_];
    layout();
    view.draw(renderer);
    layout(); // drawing may have opened the file: refresh the page count
    toolbar_.draw(renderer, *ctx_.code);
}

Workspace::Pointer ReaderWorkspace::pointerAt(float x, float y)
{
    if (toolbar_.hit(x, y))
        return Pointer::Hand;
    if (!tabs_.empty() && tabs_[active_]->overLink(x, y))
        return Pointer::Hand;
    return Pointer::Arrow;
}

void ReaderWorkspace::styleChanged()
{
    for (auto& tab : tabs_)
        tab->styleChanged();
    layout();
}

void ReaderWorkspace::imagesLoaded()
{
    for (auto& tab : tabs_)
        tab->imagesLoaded();
}

std::string ReaderWorkspace::status()
{
    return tabs_.empty() ? "Reader" : tabs_[active_]->status();
}

void ReaderWorkspace::selectTab(size_t index)
{
    if (index < tabs_.size())
        active_ = index;
    layout();
}

std::string ReaderWorkspace::tabTitle(size_t index) const
{
    return index < tabs_.size() ? tabs_[index]->title() : "Reader";
}

void ReaderWorkspace::closeTab(size_t index)
{
    if (index >= tabs_.size())
        return;
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
    if (active_ >= tabs_.size())
        active_ = tabs_.empty() ? 0 : tabs_.size() - 1;
    else if (index < active_)
        --active_;
    layout();
}

bool ReaderWorkspace::onAsync()
{
    bool changed = false;
    for (auto& tab : tabs_)
        changed |= tab->onAsync();
    return changed;
}

bool ReaderWorkspace::takeOpenRequest(std::string& target)
{
    return !tabs_.empty() && tabs_[active_]->takeOpenRequest(target);
}
