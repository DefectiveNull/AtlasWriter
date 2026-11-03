#pragma once

#include "ui/Toolbar.h"
#include "workspace/Workspace.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// HTML files and websites, one per tab. A local .html file is drawn by
// AtlasWriter's own renderer (live, if the file is open in an editor), or
// by the browser view if switched to it; websites use the browser view
// (WebView2 on Windows) or, without one, open in the system browser.
class WebWorkspace final : public Workspace
{
public:
    explicit WebWorkspace(const WorkspaceContext& context);
    ~WebWorkspace() override;

    // Local pages this tile shows (.html, .htm, .xhtml); URLs are checked
    // with isWebUrl.
    static bool handles(std::string_view path);
    static bool isWebUrl(std::string_view target); // http(s)://
    // What a typed address means: a URL as it is, an existing file, a
    // domain ("example.com" -> https://example.com), else a web search.
    static std::string addressFor(std::string typed);

    WorkspaceType type() const override { return WorkspaceType::Web; }
    void setBounds(const SDL_FRect& bounds) override;
    bool handleEvent(const SDL_Event& event) override;
    void draw(SDL_Renderer* renderer) override;
    void setFocused(bool focused) override;
    Pointer pointerAt(float x, float y) override;
    void styleChanged() override;
    void imagesLoaded() override;
    std::string status() override;

    size_t tabCount() const override { return tabs_.size(); }
    size_t activeTab() const override { return active_; }
    void selectTab(size_t index) override;
    std::string tabTitle(size_t index) const override;
    void closeTab(size_t index) override;

    bool openTarget(const std::string& target) override; // a new tab (or the tab already showing it)
    void navigate(const std::string& target);            // in the current tab
    bool onAsync() override;
    bool takeOpenRequest(std::string& target) override;
    bool takeFocusRequest() override;
    void setVisibility(bool shown, bool covered) override;
    void refresh() override;

    // The address field was clicked: the App asks for a new address.
    bool takeAddressRequest();
    std::string currentAddress() const;

    struct Page; // see WebWorkspace.cpp

private:
    void layout();
    SDL_FRect pageRect() const;
    void tool(int id);

    std::vector<std::unique_ptr<Page>> tabs_;
    size_t active_ = 0;
    SDL_FRect bounds_{};
    Toolbar toolbar_;
    bool focused_ = false;
    bool shown_ = true;
    bool covered_ = false;
    bool addressRequested_ = false;
    bool focusRequested_ = false;
    std::vector<std::string> openRequests_; // links for the App, oldest first
};
