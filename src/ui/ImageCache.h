#pragma once

#include <SDL3/SDL.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Loads images from disk into textures without blocking the UI.
//
// request() returns immediately; decoding (stb_image: PNG, JPEG, GIF's first
// frame, BMP, TGA) happens on one worker thread. When a decode finishes the
// worker pushes `eventType`; the main thread then calls processLoaded() to
// create the textures (SDL renderers are single-threaded).
//
// Huge images are halved on the worker until they fit kMaxTextureSize, so a
// 6000px photo costs a few MB of VRAM instead of ~150 MB.
class ImageCache
{
public:
    enum class State { Loading, Ready, Failed };

    struct Image
    {
        State state = State::Loading;
        SDL_Texture* texture = nullptr;
        int width = 0;  // original size in pixels (before any downscaling)
        int height = 0;
    };

    static constexpr int kMaxTextureSize = 2048;

    ImageCache(SDL_Renderer* renderer, Uint32 eventType);
    ~ImageCache();

    ImageCache(const ImageCache&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;

    // The entry for `path`, starting a load the first time it is asked for.
    // Pointers stay valid until clear().
    const Image* request(const std::string& path);

    // Main thread: turns finished decodes into textures. Returns true if any
    // image changed state (layout must be redone).
    bool processLoaded();

    // Drops every image, e.g. when another document is opened. Loads still
    // in flight are discarded when they finish.
    void clear();

private:
    struct Decoded
    {
        uint64_t generation;
        std::string path;
        std::vector<unsigned char> rgba; // empty on failure
        int width, height;               // of rgba
        int originalWidth, originalHeight;
    };

    void work();
    static Decoded decode(const std::string& path, uint64_t generation);

    SDL_Renderer* renderer_;
    Uint32 eventType_;
    std::unordered_map<std::string, Image> images_; // node-based: stable pointers

    std::mutex mutex_; // guards everything below
    std::condition_variable wake_;
    std::deque<std::pair<std::string, uint64_t>> jobs_;
    std::vector<Decoded> done_;
    uint64_t generation_ = 0;
    bool quit_ = false;
    std::thread worker_;
};
