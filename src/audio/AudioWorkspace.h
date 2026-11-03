#pragma once

#include "ui/Toolbar.h"
#include "workspace/Workspace.h"

#include <memory>
#include <string>
#include <vector>

// Audio files, one per tab: a waveform to seek in, play/pause, speed and
// volume. Only the shown tab plays.
class AudioWorkspace final : public Workspace
{
public:
    explicit AudioWorkspace(const WorkspaceContext& context);
    ~AudioWorkspace() override;

    WorkspaceType type() const override { return WorkspaceType::Audio; }
    void setBounds(const SDL_FRect& bounds) override;
    bool handleEvent(const SDL_Event& event) override;
    void draw(SDL_Renderer* renderer) override;
    Pointer pointerAt(float x, float y) override;
    void styleChanged() override { layout(); }
    std::string status() override;

    size_t tabCount() const override { return tabs_.size(); }
    size_t activeTab() const override { return active_; }
    void selectTab(size_t index) override;
    std::string tabTitle(size_t index) const override;
    void closeTab(size_t index) override;

    bool openTarget(const std::string& path) override;
    bool onAsync() override;
    Sint32 wakeMs() const override;
    void setVisibility(bool shown, bool covered) override;

    class Track; // one open file; see AudioWorkspace.cpp

private:
    void layout();
    SDL_FRect waveRect() const;
    void seekTo(float x);

    std::vector<std::unique_ptr<Track>> tabs_;
    size_t active_ = 0;
    SDL_FRect bounds_{};
    Toolbar toolbar_;
    bool scrubbing_ = false; // dragging on the waveform
};
