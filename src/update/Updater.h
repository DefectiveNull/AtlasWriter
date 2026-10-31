#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// Keeps an installed AtlasWriter up to date.
//
// On launch a background thread reads the update feed from the latest GitHub
// release. If it names a newer version, the archive is downloaded, checked
// against the feed's SHA-256 and unpacked into <install>/update/ready. The
// system curl and tar do the work, so the app carries no network code.
// applyStagedUpdate() swaps the files in once the app has closed, so an update
// never interrupts anyone's writing.
//
// Only installs update: the install scripts leave an installed.marker file,
// which a development build doesn't have.
class Updater
{
public:
    Updater() = default;
    ~Updater();

    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    // `readyEvent` is pushed when an update is waiting to be applied. An empty
    // `feedUrl` means the latest GitHub release.
    void start(Uint32 readyEvent, std::string feedUrl);
    void stop(); // cancels any download and joins the thread

    bool updateReady() const { return ready_; }

private:
    static int SDLCALL threadMain(void* self);
    void work();
    bool check(const std::filesystem::path& update);
    bool run(const std::vector<std::string>& args);

    SDL_Thread* thread_ = nullptr;
    Uint32 readyEvent_ = 0;
    std::string feedUrl_;
    std::atomic<bool> ready_ = false;
    std::atomic<bool> stopping_ = false;
    std::mutex processMutex_;
    SDL_Process* process_ = nullptr; // the running curl or tar
};

// Swaps a staged update in, restoring the old files if anything fails. Call
// once the app has closed. Returns true if an update was applied.
bool applyStagedUpdate();
