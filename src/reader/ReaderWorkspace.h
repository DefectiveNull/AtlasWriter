#pragma once

#include "ui/Toolbar.h"
#include "workspace/Workspace.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// PDF and EPUB files, one per tab, read-only. PDFs are pages rendered by
// PDFium; books show one chapter at a time as rendered HTML.
class ReaderWorkspace final : public Workspace
{
public:
    explicit ReaderWorkspace(const WorkspaceContext& context);
    ~ReaderWorkspace() override;

    // Whether a path is something this tile opens (.pdf, .epub).
    static bool handles(std::string_view path);

    WorkspaceType type() const override { return WorkspaceType::Reader; }
    void setBounds(const SDL_FRect& bounds) override;
    bool handleEvent(const SDL_Event& event) override;
    void draw(SDL_Renderer* renderer) override;
    Pointer pointerAt(float x, float y) override;
    void styleChanged() override;
    void imagesLoaded() override;
    std::string status() override;

    size_t tabCount() const override { return tabs_.size(); }
    size_t activeTab() const override { return active_; }
    void selectTab(size_t index) override;
    std::string tabTitle(size_t index) const override;
    void closeTab(size_t index) override;

    bool openTarget(const std::string& path) override;
    bool onAsync() override;
    bool takeOpenRequest(std::string& target) override;

    // One open file; see ReaderWorkspace.cpp.
    class View;

private:
    void layout();

    std::vector<std::unique_ptr<View>> tabs_;
    size_t active_ = 0;
    SDL_FRect bounds_{};
    Toolbar toolbar_;
};
