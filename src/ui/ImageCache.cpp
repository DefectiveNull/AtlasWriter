#include "ImageCache.h"

#include <stb_image.h>

#include <algorithm>
#include <cstring>

ImageCache::ImageCache(SDL_Renderer* renderer, Uint32 eventType)
    : renderer_(renderer)
    , eventType_(eventType)
    , worker_([this] { work(); })
{
}

ImageCache::~ImageCache()
{
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    worker_.join();
    for (auto& [path, image] : images_)
        SDL_DestroyTexture(image.texture);
}

const ImageCache::Image* ImageCache::request(const std::string& path)
{
    if (auto it = images_.find(path); it != images_.end())
        return &it->second;

    const Image* image = &images_.emplace(path, Image{}).first->second;
    {
        std::lock_guard lock(mutex_);
        jobs_.emplace_back(path, generation_);
    }
    wake_.notify_one();
    return image;
}

bool ImageCache::processLoaded()
{
    std::vector<Decoded> done;
    uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        done.swap(done_);
        generation = generation_;
    }

    bool changed = false;
    for (Decoded& d : done) {
        if (d.generation != generation)
            continue; // finished after clear()
        auto it = images_.find(d.path);
        if (it == images_.end())
            continue;
        Image& image = it->second;
        image.width = d.originalWidth;
        image.height = d.originalHeight;
        image.state = State::Failed;
        if (!d.rgba.empty()) {
            image.texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                              d.width, d.height);
            if (image.texture) {
                SDL_UpdateTexture(image.texture, nullptr, d.rgba.data(), d.width * 4);
                SDL_SetTextureBlendMode(image.texture, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(image.texture, SDL_SCALEMODE_LINEAR);
                image.state = State::Ready;
            } else {
                SDL_Log("Image texture for '%s' failed: %s", d.path.c_str(), SDL_GetError());
            }
        }
        changed = true;
    }
    return changed;
}

void ImageCache::clear()
{
    {
        std::lock_guard lock(mutex_);
        ++generation_;
        jobs_.clear();
        done_.clear();
    }
    for (auto& [path, image] : images_)
        SDL_DestroyTexture(image.texture);
    images_.clear();
}

void ImageCache::work()
{
    while (true) {
        std::pair<std::string, uint64_t> job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
            if (quit_)
                return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }

        Decoded decoded = decode(job.first, job.second);
        {
            std::lock_guard lock(mutex_);
            if (quit_)
                return;
            done_.push_back(std::move(decoded));
        }
        SDL_Event event{};
        event.type = eventType_;
        SDL_PushEvent(&event); // thread-safe; wakes the main loop
    }
}

ImageCache::Decoded ImageCache::decode(const std::string& path, uint64_t generation)
{
    Decoded out{generation, path, {}, 0, 0, 0, 0};

    size_t size = 0;
    void* file = SDL_LoadFile(path.c_str(), &size);
    if (!file) {
        SDL_Log("Image '%s': %s", path.c_str(), SDL_GetError());
        return out;
    }
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(file), static_cast<int>(size),
                                                  &w, &h, &channels, 4);
    SDL_free(file);
    if (!pixels) {
        SDL_Log("Image '%s': %s", path.c_str(), stbi_failure_reason());
        return out;
    }
    out.originalWidth = w;
    out.originalHeight = h;
    out.rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
    stbi_image_free(pixels);

    // Halve with a 2x2 box filter until it fits; good enough for a preview
    // that is usually shown smaller than the source anyway.
    while (w > kMaxTextureSize || h > kMaxTextureSize) {
        const int nw = std::max(1, w / 2);
        const int nh = std::max(1, h / 2);
        std::vector<unsigned char> half(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y) {
            const unsigned char* r0 = out.rgba.data() + static_cast<size_t>(std::min(2 * y, h - 1)) * w * 4;
            const unsigned char* r1 = out.rgba.data() + static_cast<size_t>(std::min(2 * y + 1, h - 1)) * w * 4;
            unsigned char* dst = half.data() + static_cast<size_t>(y) * nw * 4;
            for (int x = 0; x < nw; ++x) {
                const int x0 = std::min(2 * x, w - 1) * 4;
                const int x1 = std::min(2 * x + 1, w - 1) * 4;
                for (int c = 0; c < 4; ++c)
                    dst[x * 4 + c] = static_cast<unsigned char>((r0[x0 + c] + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) / 4);
            }
        }
        out.rgba.swap(half);
        w = nw;
        h = nh;
    }
    out.width = w;
    out.height = h;
    return out;
}
