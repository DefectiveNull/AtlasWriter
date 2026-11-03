#include "web/WebWorkspace.h"

#include "embed/Embed.h"
#include "markdown/Html.h"
#include "markdown/MarkdownView.h"
#include "text/Font.h"
#include "text/TextRenderer.h"
#include "ui/Draw.h"
#include "web/WebView.h"
#include "workspace/Document.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace {

enum ToolId : int {
    kToolBack = 1,
    kToolForward,
    kToolReload,
    kToolAddress,
    kToolMode,
    kToolExternal,
};

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

SDL_Time modifyTime(const std::string& path)
{
    SDL_PathInfo info{};
    return SDL_GetPathInfo(path.c_str(), &info) ? info.modify_time : 0;
}

std::string directoryOf(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// file:///C:/My%20Pages/index.html
std::string fileUrl(std::string path)
{
    for (char& c : path) {
        if (c == '\\')
            c = '/';
    }
    if (!path.empty() && path[0] != '/')
        path.insert(0, "/");
    return "file://" + embed::encodeLinkPath(path);
}

} // namespace

struct WebWorkspace::Page
{
    // Local pages visited in this tab (AtlasWriter's renderer); the browser
    // view keeps its own history.
    std::vector<std::string> history;
    size_t at = 0;
    bool browser = false; // shown in the browser view
    std::unique_ptr<WebView> native;
    MarkdownView view;
    uint64_t version = 0;

    // What the renderer shows, to notice changes
    std::string shownPath;
    const Document* shownDoc = nullptr;
    uint64_t shownKey = 0;
    SDL_Time shownTime = -1;
    SDL_Time browserTime = 0; // the local file's time when the browser loaded it
    std::string title;
    std::string error;

    const std::string& target() const { return history[at]; }
    bool web() const { return WebWorkspace::isWebUrl(target()); }
};

WebWorkspace::WebWorkspace(const WorkspaceContext& context)
    : Workspace(context)
{
}

WebWorkspace::~WebWorkspace() = default;

bool WebWorkspace::handles(std::string_view path)
{
    const std::string ext = lowerExtension(path);
    return ext == "html" || ext == "htm" || ext == "xhtml";
}

bool WebWorkspace::isWebUrl(std::string_view target)
{
    const auto starts = [&](std::string_view prefix) {
        return target.size() >= prefix.size() && SDL_strncasecmp(target.data(), prefix.data(), prefix.size()) == 0;
    };
    return starts("http://") || starts("https://");
}

std::string WebWorkspace::addressFor(std::string typed)
{
    while (!typed.empty() && (typed.front() == ' ' || typed.front() == '\t'))
        typed.erase(typed.begin());
    while (!typed.empty() && (typed.back() == ' ' || typed.back() == '\t'))
        typed.pop_back();
    if (typed.empty())
        return {};
    if (typed.find("://") != std::string::npos || typed.rfind("about:", 0) == 0)
        return typed;
    SDL_PathInfo info{};
    if (SDL_GetPathInfo(typed.c_str(), &info))
        return typed; // a file on disk
    const bool domain = typed.find(' ') == std::string::npos && typed.find('.') != std::string::npos;
    if (domain || typed.rfind("localhost", 0) == 0)
        return "https://" + typed;
    std::string query;
    for (unsigned char c : typed) {
        if (SDL_isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            query += static_cast<char>(c);
        } else if (c == ' ') {
            query += '+';
        } else {
            char hex[4];
            SDL_snprintf(hex, sizeof(hex), "%%%02X", c);
            query += hex;
        }
    }
    return "https://duckduckgo.com/?q=" + query;
}

bool WebWorkspace::openTarget(const std::string& target)
{
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i]->target() == target) {
            selectTab(i);
            return true;
        }
    }
    auto page = std::make_unique<Page>();
    page->view.setFonts(ctx_.markdown);
    page->history.push_back(target);
    const size_t at = tabs_.empty() ? 0 : active_ + 1;
    tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(at), std::move(page));
    active_ = at;
    navigate(target);
    return true;
}

void WebWorkspace::navigate(const std::string& target)
{
    if (target.empty())
        return;
    if (tabs_.empty()) {
        openTarget(target);
        return;
    }
    Page& page = *tabs_[active_];
    if (page.target() != target) {
        page.history.resize(page.at + 1);
        page.history.push_back(target);
        page.at = page.history.size() - 1;
    }
    page.error.clear();
    const bool web = isWebUrl(target);
    if (web)
        page.browser = true;
    if (page.browser && WebView::available()) {
        if (!page.native)
            page.native = std::make_unique<WebView>(ctx_.window, ctx_.asyncEvent, ctx_.isAppShortcut);
        page.native->navigate(web ? target : fileUrl(target));
        page.browserTime = web ? 0 : modifyTime(target);
    }
    if (!web)
        page.view.scrollToTop();
    layout();
}

SDL_FRect WebWorkspace::pageRect() const
{
    const float bar = Toolbar::heightFor(*ctx_.code);
    return {bounds_.x, bounds_.y + bar, bounds_.w, std::max(0.0f, bounds_.h - bar)};
}

void WebWorkspace::setBounds(const SDL_FRect& bounds)
{
    bounds_ = bounds;
    layout();
}

std::string WebWorkspace::currentAddress() const
{
    if (tabs_.empty())
        return {};
    const Page& page = *tabs_[active_];
    if (page.browser && page.native && !page.native->url().empty())
        return page.native->url();
    return page.target();
}

void WebWorkspace::layout()
{
    std::vector<Toolbar::Item> items;
    if (!tabs_.empty()) {
        Page& page = *tabs_[active_];
        const bool native = page.browser && page.native && WebView::available();
        const bool back = native ? page.native->canGoBack() || page.at > 0 : page.at > 0;
        const bool forward = native ? page.native->canGoForward() : page.at + 1 < page.history.size();
        items.push_back({kToolBack, "", Toolbar::Icon::Back, back});
        items.push_back({kToolForward, "", Toolbar::Icon::Forward, forward});
        items.push_back({kToolReload, "", Toolbar::Icon::Reload});
        std::string address = currentAddress();
        if (native && page.native->loading())
            address = "Loading  " + address;
        items.push_back({kToolAddress, address, Toolbar::Icon::None, true, false, true, true});
        if (!page.web() && WebView::available())
            items.push_back({kToolMode, page.browser ? "Simple view" : "Browser view", Toolbar::Icon::None, true, true});
        items.push_back({kToolExternal, "Open in browser", Toolbar::Icon::None, true, true});
    }
    toolbar_.set({bounds_.x, bounds_.y, bounds_.w, Toolbar::heightFor(*ctx_.code)}, *ctx_.code, std::move(items));

    // Browser views: only the current tab's, and only while nothing covers it.
    const SDL_FRect area = pageRect();
    for (size_t i = 0; i < tabs_.size(); ++i) {
        Page& page = *tabs_[i];
        if (!page.native)
            continue;
        const bool visible = i == active_ && page.browser && shown_ && !covered_ && area.w > 1.0f && area.h > 1.0f;
        if (visible)
            page.native->setBounds(area);
        page.native->setVisible(visible);
    }
}

void WebWorkspace::tool(int id)
{
    if (tabs_.empty())
        return;
    Page& page = *tabs_[active_];
    const bool native = page.browser && page.native;
    switch (id) {
    case kToolBack:
        if (native && page.native->canGoBack()) {
            page.native->back();
        } else if (page.at > 0) {
            --page.at;
            page.browser = page.web();
            if (page.browser)
                navigate(page.target());
        }
        break;
    case kToolForward:
        if (native) {
            page.native->forward();
        } else if (page.at + 1 < page.history.size()) {
            ++page.at;
            page.browser = page.web();
            if (page.browser)
                navigate(page.target());
        }
        break;
    case kToolReload:
        if (native)
            page.native->reload();
        else
            page.shownTime = -1; // read the file again
        break;
    case kToolAddress:
        addressRequested_ = true;
        break;
    case kToolMode:
        page.browser = !page.browser;
        if (page.browser)
            navigate(page.target());
        break;
    case kToolExternal: {
        const std::string address = currentAddress();
        SDL_OpenURL((isWebUrl(address) || address.find("://") != std::string::npos ? address : fileUrl(address)).c_str());
        break;
    }
    default:
        break;
    }
    layout();
}

bool WebWorkspace::handleEvent(const SDL_Event& event)
{
    if (tabs_.empty())
        return false;
    layout();
    Page& page = *tabs_[active_];
    if (event.type == SDL_EVENT_MOUSE_MOTION && toolbar_.hover(event.motion.x, event.motion.y))
        return true;
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        if (const int id = toolbar_.hit(event.button.x, event.button.y)) {
            tool(id);
            return true;
        }
    }
    if (page.browser)
        return false; // the browser view handles its own input
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F5) {
        tool(kToolReload);
        return true;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && (event.key.mod & SDL_KMOD_ALT)) {
        if (event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT) {
            tool(event.key.key == SDLK_LEFT ? kToolBack : kToolForward);
            return true;
        }
    }
    bool redraw = page.view.handleEvent(event);

    // A link in a local page: other local pages open in this tab, websites
    // too when there's a browser view; anything else goes to the App.
    std::string url;
    if (page.view.takeLinkClick(url)) {
        const std::string target = page.view.resolveLink(url);
        if (!target.empty()) {
            if (handles(target) || (isWebUrl(target) && WebView::available()))
                navigate(target);
            else
                openRequests_.push_back(target);
        }
        redraw = true;
    }
    return redraw;
}

void WebWorkspace::draw(SDL_Renderer* renderer)
{
    const SDL_FRect area = pageRect();
    if (tabs_.empty()) {
        ui::fillRect(renderer, bounds_, theme::kEditorBg);
        ui::drawMessage(renderer, *ctx_.code, bounds_,
                        {"No page open", "Click a link to an .html file or a website, drop one here,",
                         "or click the address bar of a page to type one."});
        return;
    }
    layout();
    Page& page = *tabs_[active_];
    TextRenderer& text = *ctx_.code;

    if (page.browser) {
        ui::fillRect(renderer, area, theme::kEditorBg);
        if (!WebView::available() || !page.native) {
            ui::drawMessage(renderer, text, area,
                            {"Websites open in your web browser on this system.",
                             "Use \"Open in browser\" above to see " + page.target()});
        } else if (!page.native->error().empty()) {
            std::vector<std::string> lines = {"The page can't be shown here"};
            const std::string e = page.native->error();
            size_t start = 0;
            while (start <= e.size()) {
                const size_t end = std::min(e.find('\n', start), e.size());
                lines.push_back(e.substr(start, end - start));
                start = end + 1;
            }
            ui::drawMessage(renderer, text, area, lines);
        } else if (covered_ || !page.native->ready()) {
            ui::drawMessage(renderer, text, area, {page.native->ready() ? currentAddress() : "Starting the browser view..."});
        }
        // A local file saved since the browser loaded it: show the new version.
        if (!page.web() && page.native && page.native->ready()) {
            const SDL_Time now = modifyTime(page.target());
            if (now != page.browserTime) {
                page.browserTime = now;
                page.native->reload();
            }
        }
        toolbar_.draw(renderer, text);
        return;
    }

    // AtlasWriter's renderer: the open document's text if there is one,
    // else the file, read again when it changes.
    const std::string& path = page.target();
    const std::shared_ptr<Document> doc = ctx_.findDocument ? ctx_.findDocument(path) : nullptr;
    std::string html;
    bool reload = path != page.shownPath;
    if (doc) {
        reload = reload || doc.get() != page.shownDoc || doc->buffer().version() != page.shownKey;
        if (reload)
            html = doc->buffer().toString();
    } else {
        const SDL_Time time = modifyTime(path);
        reload = reload || page.shownDoc || time != page.shownTime;
        if (reload) {
            page.shownTime = time;
            size_t size = 0;
            if (void* data = SDL_LoadFile(path.c_str(), &size)) {
                html.assign(static_cast<const char*>(data), size);
                SDL_free(data);
                page.error.clear();
            } else {
                page.error = "Could not read " + path;
            }
        }
    }
    if (reload) {
        if (path != page.shownPath) {
            page.view.setImages(ctx_.images, directoryOf(path), ctx_.displayScale);
            page.view.expandAll();
        }
        page.shownPath = path;
        page.shownDoc = doc.get();
        page.shownKey = doc ? doc->buffer().version() : 0;
        page.title = md::htmlTitle(html);
        page.view.setSource(html, ++page.version, true);
    }
    page.view.setBounds(area);
    if (!page.error.empty()) {
        ui::fillRect(renderer, area, theme::kEditorBg);
        ui::drawMessage(renderer, text, area, {page.error});
    } else {
        page.view.draw(renderer);
    }
    toolbar_.draw(renderer, text);
}

void WebWorkspace::setFocused(bool focused)
{
    const bool gained = focused && !focused_;
    focused_ = focused;
    if (gained && !tabs_.empty()) {
        Page& page = *tabs_[active_];
        if (page.browser && page.native && page.native->ready())
            page.native->focus();
    }
}

Workspace::Pointer WebWorkspace::pointerAt(float x, float y)
{
    if (toolbar_.hit(x, y))
        return Pointer::Hand;
    if (!tabs_.empty() && !tabs_[active_]->browser && tabs_[active_]->view.isOverClickable(x, y))
        return Pointer::Hand;
    return Pointer::Arrow;
}

void WebWorkspace::styleChanged()
{
    for (auto& page : tabs_) {
        page->view.setFonts(ctx_.markdown);
        page->shownPath.clear(); // re-applies the image scale too
    }
    layout();
}

void WebWorkspace::imagesLoaded()
{
    for (auto& page : tabs_)
        page->view.invalidateLayout();
}

std::string WebWorkspace::status()
{
    if (tabs_.empty())
        return "Web";
    const Page& page = *tabs_[active_];
    if (page.browser && page.native && page.native->loading())
        return "Web \xC2\xB7 loading...";
    return page.browser ? "Web" : "Web \xC2\xB7 simple view";
}

void WebWorkspace::selectTab(size_t index)
{
    if (index < tabs_.size())
        active_ = index;
    layout();
}

std::string WebWorkspace::tabTitle(size_t index) const
{
    if (index >= tabs_.size())
        return "Web";
    const Page& page = *tabs_[index];
    if (page.browser && page.native && !page.native->title().empty())
        return page.native->title();
    if (!page.browser && !page.title.empty())
        return page.title;
    return page.web() ? embed::urlLabel(page.target()) : embed::fileName(page.target());
}

void WebWorkspace::closeTab(size_t index)
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

bool WebWorkspace::onAsync()
{
    bool changed = false;
    std::vector<std::string> windows;
    for (auto& page : tabs_) {
        if (!page->native)
            continue;
        changed = true; // title, address or loading state
        if (page->native->takeFocused())
            focusRequested_ = true;
        std::string url;
        while (page->native->takeNewWindow(url))
            windows.push_back(url);
    }
    for (const std::string& url : windows)
        openTarget(url);
    if (changed)
        layout();
    return changed;
}

bool WebWorkspace::takeOpenRequest(std::string& target)
{
    if (openRequests_.empty())
        return false;
    target = std::move(openRequests_.front());
    openRequests_.erase(openRequests_.begin());
    return true;
}

bool WebWorkspace::takeFocusRequest()
{
    return std::exchange(focusRequested_, false);
}

bool WebWorkspace::takeAddressRequest()
{
    return std::exchange(addressRequested_, false);
}

void WebWorkspace::setVisibility(bool shown, bool covered)
{
    if (shown == shown_ && covered == covered_)
        return;
    shown_ = shown;
    covered_ = covered;
    layout();
}

void WebWorkspace::refresh()
{
    for (auto& page : tabs_)
        page->shownTime = -1;
}
