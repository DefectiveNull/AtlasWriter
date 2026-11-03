#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <memory>
#include <string>

// A real browser view: WebView2 (Edge's engine) as a child window over part
// of AtlasWriter's window. Only on Windows builds with WebView2; elsewhere
// available() is false and web tiles offer the system browser instead.
//
// A child window always sits on top of what SDL draws, so the owner hides
// it while menus or popups are open.
class WebView
{
public:
    static bool available();
    // Gives the keyboard back to AtlasWriter's own window if a page has it
    // (called when something else in the window is clicked).
    static void reclaimFocus(SDL_Window* window);

    // `event` is pushed whenever there's news (loaded, title, focus); keys
    // that `isShortcut` accepts are passed to the App as asyncEvent with
    // code kAsyncShortcut instead of going to the page.
    WebView(SDL_Window* window, Uint32 event, std::function<bool(SDL_Keycode, SDL_Keymod)> isShortcut);
    ~WebView();
    WebView(const WebView&) = delete;
    WebView& operator=(const WebView&) = delete;

    void navigate(const std::string& url);
    void back();
    void forward();
    void reload();
    void focus(); // moves the keyboard into the page

    // In render coordinates (what SDL draws in).
    void setBounds(const SDL_FRect& rect);
    void setVisible(bool visible);

    bool ready() const;   // the browser has started
    bool loading() const;
    bool canGoBack() const;
    bool canGoForward() const;
    std::string url() const;
    std::string title() const;
    std::string error() const; // why the browser couldn't start; empty if fine

    // The page was clicked into (its tile should take focus), once.
    bool takeFocused();
    // A link asked for a new window: its URL, once per request.
    bool takeNewWindow(std::string& url);

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};
