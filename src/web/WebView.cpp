#include "web/WebView.h"

#include "workspace/Workspace.h" // kAsyncShortcut

#include <cstdint>
#include <utility>
#include <vector>

#if defined(ATLASWRITER_WEBVIEW2)

#define NOMINMAX
#include <windows.h>
#include <objbase.h>

#include <WebView2.h>
#include <wrl.h>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

std::wstring wide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(const wchar_t* s)
{
    if (!s || !*s)
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// Takes a string WebView2 allocated.
std::string take(LPWSTR s)
{
    std::string out = narrow(s);
    CoTaskMemFree(s);
    return out;
}

HWND windowHandle(SDL_Window* window)
{
    return static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

// Windows virtual keys to SDL keycodes, for the keys AtlasWriter's
// shortcuts use. Others aren't passed on.
SDL_Keycode keycodeFor(UINT vk)
{
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<SDL_Keycode>(SDLK_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9')
        return static_cast<SDL_Keycode>(SDLK_0 + (vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F12)
        return static_cast<SDL_Keycode>(SDLK_F1 + (vk - VK_F1));
    switch (vk) {
    case VK_LEFT: return SDLK_LEFT;
    case VK_RIGHT: return SDLK_RIGHT;
    case VK_UP: return SDLK_UP;
    case VK_DOWN: return SDLK_DOWN;
    case VK_TAB: return SDLK_TAB;
    case VK_BACK: return SDLK_BACKSPACE;
    case VK_OEM_5: return SDLK_BACKSLASH;
    case VK_OEM_MINUS: return SDLK_MINUS;
    case VK_OEM_PLUS: return SDLK_EQUALS;
    case VK_OEM_COMMA: return SDLK_COMMA;
    case VK_OEM_3: return SDLK_GRAVE;
    default: return SDLK_UNKNOWN;
    }
}

} // namespace

struct WebView::Impl : std::enable_shared_from_this<WebView::Impl>
{
    HWND parent = nullptr;
    SDL_Window* window = nullptr;
    Uint32 event = 0;
    std::function<bool(SDL_Keycode, SDL_Keymod)> isShortcut;
    bool destroyed = false;

    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;
    std::string pendingUrl;
    RECT bounds{};
    bool haveBounds = false;
    bool visible = false;

    bool loading = false;
    bool canGoBack = false;
    bool canGoForward = false;
    std::string url;
    std::string title;
    std::string error;
    bool focused = false;
    std::vector<std::string> newWindows;

    void notify() const
    {
        SDL_Event e{};
        e.type = event;
        SDL_PushEvent(&e);
    }

    void refreshState()
    {
        if (!view)
            return;
        BOOL b = FALSE;
        view->get_CanGoBack(&b);
        canGoBack = b != FALSE;
        view->get_CanGoForward(&b);
        canGoForward = b != FALSE;
        LPWSTR s = nullptr;
        if (SUCCEEDED(view->get_Source(&s)))
            url = take(s);
        if (SUCCEEDED(view->get_DocumentTitle(&s)))
            title = take(s);
    }

    void applyBounds()
    {
        if (controller && haveBounds)
            controller->put_Bounds(bounds);
    }

    void created(ICoreWebView2Controller* made);
};

namespace {

// One browser environment (a set of processes) serves every view.
struct Environment
{
    ComPtr<ICoreWebView2Environment> env;
    bool requested = false;
    std::string error;
    std::vector<std::shared_ptr<WebView::Impl>> waiting;
};

Environment& environment()
{
    static Environment instance;
    return instance;
}

void createController(const std::shared_ptr<WebView::Impl>& impl)
{
    Environment& e = environment();
    const HRESULT hr = e.env->CreateCoreWebView2Controller(
        impl->parent, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                          [impl](HRESULT result, ICoreWebView2Controller* made) -> HRESULT {
                              if (impl->destroyed) {
                                  if (made)
                                      made->Close();
                                  return S_OK;
                              }
                              if (FAILED(result) || !made) {
                                  char text[96];
                                  SDL_snprintf(text, sizeof(text), "The page view could not be created (0x%08lX).",
                                               static_cast<unsigned long>(result));
                                  impl->error = text;
                                  impl->notify();
                                  return S_OK;
                              }
                              impl->created(made);
                              return S_OK;
                          })
                          .Get());
    if (FAILED(hr)) {
        impl->error = "The page view could not be created.";
        impl->notify();
    }
}

void requestEnvironment(const std::shared_ptr<WebView::Impl>& impl)
{
    Environment& e = environment();
    if (e.env) {
        createController(impl);
        return;
    }
    if (!e.error.empty()) {
        impl->error = e.error;
        impl->notify();
        return;
    }
    e.waiting.push_back(impl);
    if (e.requested)
        return;
    e.requested = true;

    // Pages need an apartment-threaded COM on this (the UI) thread.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    (void)com; // S_FALSE or RPC_E_CHANGED_MODE: already set up by someone else

    // Cookies and caches live with AtlasWriter's other local data, not next
    // to the executable.
    std::wstring folder;
    if (const char* local = SDL_getenv("LOCALAPPDATA"))
        folder = wide(std::string(local) + "\\AtlasWriter\\WebView2");

    const auto fail = [](const std::string& message) {
        Environment& env = environment();
        env.error = message;
        for (const auto& waiting : env.waiting) {
            waiting->error = message;
            waiting->notify();
        }
        env.waiting.clear();
    };

    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, folder.empty() ? nullptr : folder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [fail](HRESULT result, ICoreWebView2Environment* made) -> HRESULT {
                Environment& env = environment();
                if (FAILED(result) || !made) {
                    fail("The Microsoft Edge WebView2 Runtime could not be started.");
                    return S_OK;
                }
                env.env = made;
                std::vector<std::shared_ptr<WebView::Impl>> waiting = std::move(env.waiting);
                env.waiting.clear();
                for (const auto& w : waiting) {
                    if (!w->destroyed)
                        createController(w);
                }
                return S_OK;
            })
            .Get());
    if (FAILED(hr)) {
        fail(hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
                 ? "Web pages need the Microsoft Edge WebView2 Runtime, which isn't installed.\n"
                   "Get it from microsoft.com/edge/webview2, or open the page in your browser."
                 : "The Microsoft Edge WebView2 Runtime could not be started.");
    }
}

} // namespace

void WebView::Impl::created(ICoreWebView2Controller* made)
{
    controller = made;
    controller->get_CoreWebView2(&view);
    if (!view) {
        error = "The page view could not be created.";
        notify();
        return;
    }
    // A dark backdrop while pages load, like the rest of the window.
    ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(controller.As(&controller2)))
        controller2->put_DefaultBackgroundColor({255, 30, 30, 34});

    EventRegistrationToken token{};
    const std::weak_ptr<Impl> weak = weak_from_this(); // events can outlive a closed tab
    view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                                     [weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*) -> HRESULT {
                                         const std::shared_ptr<Impl> self = weak.lock();
                                         if (!self || self->destroyed)
                                             return S_OK;
                                         self->loading = true;
                                         self->notify();
                                         return S_OK;
                                     })
                                     .Get(),
                                 &token);
    view->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                      [weak](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                          const std::shared_ptr<Impl> self = weak.lock();
                                          if (!self || self->destroyed)
                                              return S_OK;
                                          self->loading = false;
                                          self->refreshState();
                                          self->notify();
                                          return S_OK;
                                      })
                                      .Get(),
                                  &token);
    view->add_SourceChanged(Callback<ICoreWebView2SourceChangedEventHandler>(
                                [weak](ICoreWebView2*, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                                    const std::shared_ptr<Impl> self = weak.lock();
                                    if (!self || self->destroyed)
                                        return S_OK;
                                    self->refreshState();
                                    self->notify();
                                    return S_OK;
                                })
                                .Get(),
                            &token);
    view->add_DocumentTitleChanged(Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                                       [weak](ICoreWebView2*, IUnknown*) -> HRESULT {
                                           const std::shared_ptr<Impl> self = weak.lock();
                                           if (!self || self->destroyed)
                                               return S_OK;
                                           self->refreshState();
                                           self->notify();
                                           return S_OK;
                                       })
                                       .Get(),
                                   &token);
    view->add_HistoryChanged(Callback<ICoreWebView2HistoryChangedEventHandler>(
                                 [weak](ICoreWebView2*, IUnknown*) -> HRESULT {
                                     const std::shared_ptr<Impl> self = weak.lock();
                                     if (!self || self->destroyed)
                                         return S_OK;
                                     self->refreshState();
                                     self->notify();
                                     return S_OK;
                                 })
                                 .Get(),
                             &token);
    // Pages that open windows get a new tab instead.
    view->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                     [weak](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                         const std::shared_ptr<Impl> self = weak.lock();
                                         if (!self || self->destroyed)
                                             return S_OK;
                                         LPWSTR uri = nullptr;
                                         if (SUCCEEDED(args->get_Uri(&uri)))
                                             self->newWindows.push_back(take(uri));
                                         args->put_Handled(TRUE);
                                         self->notify();
                                         return S_OK;
                                     })
                                     .Get(),
                                 &token);
    controller->add_GotFocus(Callback<ICoreWebView2FocusChangedEventHandler>(
                                 [weak](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
                                     const std::shared_ptr<Impl> self = weak.lock();
                                     if (!self || self->destroyed)
                                         return S_OK;
                                     self->focused = true;
                                     self->notify();
                                     return S_OK;
                                 })
                                 .Get(),
                             &token);
    // AtlasWriter's shortcuts (tile keys, tabs) still work inside a page.
    controller->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [weak](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                const std::shared_ptr<Impl> self = weak.lock();
                if (!self || self->destroyed)
                    return S_OK;
                COREWEBVIEW2_KEY_EVENT_KIND kind{};
                UINT vk = 0;
                args->get_KeyEventKind(&kind);
                args->get_VirtualKey(&vk);
                if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
                    return S_OK;
                const SDL_Keycode key = keycodeFor(vk);
                if (key == SDLK_UNKNOWN || !self->isShortcut)
                    return S_OK;
                SDL_Keymod mod = SDL_KMOD_NONE;
                if (GetKeyState(VK_CONTROL) < 0)
                    mod |= SDL_KMOD_LCTRL;
                if (GetKeyState(VK_SHIFT) < 0)
                    mod |= SDL_KMOD_LSHIFT;
                if (GetKeyState(VK_MENU) < 0)
                    mod |= SDL_KMOD_LALT;
                const bool fkey = key >= SDLK_F1 && key <= SDLK_F12;
                if (!(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) && !fkey)
                    return S_OK;
                if (!self->isShortcut(key, mod))
                    return S_OK;
                args->put_Handled(TRUE);
                SDL_Event e{};
                e.type = self->event;
                e.user.code = kAsyncShortcut;
                e.user.data1 = reinterpret_cast<void*>(static_cast<uintptr_t>(key));
                e.user.data2 = reinterpret_cast<void*>(static_cast<uintptr_t>(mod));
                SDL_PushEvent(&e);
                return S_OK;
            })
            .Get(),
        &token);

    applyBounds();
    controller->put_IsVisible(visible ? TRUE : FALSE);
    if (!pendingUrl.empty()) {
        view->Navigate(wide(pendingUrl).c_str());
        pendingUrl.clear();
    }
    notify();
}

bool WebView::available()
{
    return true;
}

void WebView::reclaimFocus(SDL_Window* window)
{
    const HWND hwnd = windowHandle(window);
    const HWND focus = GetFocus();
    if (hwnd && focus && focus != hwnd && IsChild(hwnd, focus))
        SetFocus(hwnd);
}

WebView::WebView(SDL_Window* window, Uint32 event, std::function<bool(SDL_Keycode, SDL_Keymod)> isShortcut)
    : impl_(std::make_shared<Impl>())
{
    impl_->window = window;
    impl_->parent = windowHandle(window);
    impl_->event = event;
    impl_->isShortcut = std::move(isShortcut);
    if (!impl_->parent) {
        impl_->error = "The window has no native handle for a page view.";
        return;
    }
    // Child windows must not be painted over by the parent's drawing.
    const LONG_PTR style = GetWindowLongPtrW(impl_->parent, GWL_STYLE);
    if (!(style & WS_CLIPCHILDREN))
        SetWindowLongPtrW(impl_->parent, GWL_STYLE, style | WS_CLIPCHILDREN);
    requestEnvironment(impl_);
}

WebView::~WebView()
{
    impl_->destroyed = true;
    if (impl_->controller) {
        impl_->controller->Close();
        impl_->controller.Reset();
        impl_->view.Reset();
    }
}

void WebView::navigate(const std::string& url)
{
    if (impl_->view)
        impl_->view->Navigate(wide(url).c_str());
    else
        impl_->pendingUrl = url;
    impl_->url = url;
}

void WebView::back()
{
    if (impl_->view)
        impl_->view->GoBack();
}

void WebView::forward()
{
    if (impl_->view)
        impl_->view->GoForward();
}

void WebView::reload()
{
    if (impl_->view)
        impl_->view->Reload();
}

void WebView::focus()
{
    if (impl_->controller)
        impl_->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void WebView::setBounds(const SDL_FRect& rect)
{
    // Render coordinates are pixels; the child window wants the parent's
    // client coordinates, which match unless Windows is scaling the window.
    int pixelW = 0, pixelH = 0;
    SDL_GetWindowSizeInPixels(impl_->window, &pixelW, &pixelH);
    RECT client{};
    GetClientRect(impl_->parent, &client);
    const float sx = pixelW > 0 ? static_cast<float>(client.right) / static_cast<float>(pixelW) : 1.0f;
    const float sy = pixelH > 0 ? static_cast<float>(client.bottom) / static_cast<float>(pixelH) : 1.0f;
    const RECT r{static_cast<LONG>(rect.x * sx), static_cast<LONG>(rect.y * sy),
                 static_cast<LONG>((rect.x + rect.w) * sx), static_cast<LONG>((rect.y + rect.h) * sy)};
    if (impl_->haveBounds && EqualRect(&r, &impl_->bounds))
        return;
    impl_->bounds = r;
    impl_->haveBounds = true;
    impl_->applyBounds();
}

void WebView::setVisible(bool visible)
{
    if (visible == impl_->visible)
        return;
    impl_->visible = visible;
    if (impl_->controller)
        impl_->controller->put_IsVisible(visible ? TRUE : FALSE);
}

bool WebView::ready() const
{
    return impl_->view != nullptr;
}

bool WebView::loading() const
{
    return impl_->loading;
}

bool WebView::canGoBack() const
{
    return impl_->canGoBack;
}

bool WebView::canGoForward() const
{
    return impl_->canGoForward;
}

std::string WebView::url() const
{
    return impl_->url;
}

std::string WebView::title() const
{
    return impl_->title;
}

std::string WebView::error() const
{
    return impl_->error;
}

bool WebView::takeFocused()
{
    return std::exchange(impl_->focused, false);
}

bool WebView::takeNewWindow(std::string& url)
{
    if (impl_->newWindows.empty())
        return false;
    url = std::move(impl_->newWindows.front());
    impl_->newWindows.erase(impl_->newWindows.begin());
    return true;
}

#else // no native web view on this platform

struct WebView::Impl
{
    std::string url;
};

bool WebView::available()
{
    return false;
}

void WebView::reclaimFocus(SDL_Window*) {}

WebView::WebView(SDL_Window*, Uint32, std::function<bool(SDL_Keycode, SDL_Keymod)>)
    : impl_(std::make_shared<Impl>())
{
}

WebView::~WebView() = default;

void WebView::navigate(const std::string& url)
{
    impl_->url = url;
}

void WebView::back() {}
void WebView::forward() {}
void WebView::reload() {}
void WebView::focus() {}
void WebView::setBounds(const SDL_FRect&) {}
void WebView::setVisible(bool) {}

bool WebView::ready() const
{
    return false;
}

bool WebView::loading() const
{
    return false;
}

bool WebView::canGoBack() const
{
    return false;
}

bool WebView::canGoForward() const
{
    return false;
}

std::string WebView::url() const
{
    return impl_->url;
}

std::string WebView::title() const
{
    return {};
}

std::string WebView::error() const
{
    return "Web pages open in your web browser on this system.";
}

bool WebView::takeFocused()
{
    return false;
}

bool WebView::takeNewWindow(std::string&)
{
    return false;
}

#endif
