#include "reader/Pdf.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace pdf {

namespace {

// The few PDFium declarations used (fpdfview.h), so building needs no
// PDFium headers; the library is found at run time.
using FPDF_DOCUMENT = void*;
using FPDF_PAGE = void*;
using FPDF_BITMAP = void*;

struct FileAccess // FPDF_FILEACCESS
{
    unsigned long fileLength;
    int (*getBlock)(void* param, unsigned long position, unsigned char* buffer, unsigned long size);
    void* param;
};

constexpr int kFormatBGRA = 4;   // FPDFBitmap_BGRA
constexpr int kRenderAnnotations = 0x01; // FPDF_ANNOT

struct Api
{
    void (*initLibrary)();
    void (*destroyLibrary)();
    FPDF_DOCUMENT (*loadCustomDocument)(FileAccess* access, const char* password);
    unsigned long (*getLastError)();
    int (*getPageCount)(FPDF_DOCUMENT document);
    int (*getPageSizeByIndex)(FPDF_DOCUMENT document, int index, double* width, double* height);
    FPDF_PAGE (*loadPage)(FPDF_DOCUMENT document, int index);
    void (*closePage)(FPDF_PAGE page);
    void (*closeDocument)(FPDF_DOCUMENT document);
    FPDF_BITMAP (*bitmapCreateEx)(int width, int height, int format, void* firstScan, int stride);
    int (*bitmapFillRect)(FPDF_BITMAP bitmap, int left, int top, int width, int height, unsigned long color);
    void* (*bitmapGetBuffer)(FPDF_BITMAP bitmap);
    int (*bitmapGetStride)(FPDF_BITMAP bitmap);
    void (*bitmapDestroy)(FPDF_BITMAP bitmap);
    void (*renderPageBitmap)(FPDF_BITMAP bitmap, FPDF_PAGE page, int x, int y, int width, int height, int rotate,
                             int flags);
};

#if defined(SDL_PLATFORM_WINDOWS)
constexpr const char* kLibraryName = "pdfium.dll";
#elif defined(SDL_PLATFORM_APPLE)
constexpr const char* kLibraryName = "libpdfium.dylib";
#else
constexpr const char* kLibraryName = "libpdfium.so";
#endif

std::string describeError(unsigned long code)
{
    switch (code) {
    case 2: return "The file could not be opened.";
    case 3: return "This is not a PDF file, or it is damaged.";
    case 4: return "This PDF is protected by a password, which AtlasWriter can't ask for yet.";
    case 5: return "This PDF uses a security handler PDFium doesn't support.";
    default: return "PDFium could not read this file.";
    }
}

} // namespace

struct State
{
    std::mutex mutex;
    const std::string path;
    const Uint32 event;
    Document::Status status = Document::Status::Loading;
    std::string error;
    std::vector<PageSize> pages;
    std::vector<std::pair<int, int>> wanted;
    std::vector<Bitmap> done;
    bool closed = false;

    // Worker only
    FPDF_DOCUMENT document = nullptr;
    SDL_IOStream* io = nullptr;
    FileAccess access{};

    State(std::string p, Uint32 e)
        : path(std::move(p)), event(e)
    {
    }
};

namespace {

class Worker
{
public:
    void post(const std::shared_ptr<State>& state)
    {
        std::lock_guard lock(mutex_);
        if (!thread_.joinable() && !stopping_)
            thread_ = std::thread([this] { run(); });
        queue_.push_back(state);
        wake_.notify_one();
    }

    void stop()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            wake_.notify_one();
        }
        if (thread_.joinable())
            thread_.join();
        for (const auto& state : open_)
            close(*state);
        open_.clear();
        if (library_) {
            if (initialized_)
                api_.destroyLibrary();
            SDL_UnloadObject(library_);
            library_ = nullptr;
        }
    }

private:
    void run()
    {
        for (;;) {
            std::shared_ptr<State> state;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                if (stopping_)
                    return;
                state = std::move(queue_.front());
                queue_.pop_front();
            }
            process(state);
        }
    }

    // Loads PDFium on first use. Its error is kept for every later document.
    bool loadLibrary(std::string& error)
    {
        if (initialized_)
            return true;
        if (!libraryError_.empty()) {
            error = libraryError_;
            return false;
        }
        const std::string path = std::string(SDL_GetBasePath()) + kLibraryName;
        library_ = SDL_LoadObject(path.c_str());
        if (!library_) {
            libraryError_ = std::string("PDF support needs ") + kLibraryName
                          + " next to AtlasWriter, and it could not be loaded:\n" + SDL_GetError();
            error = libraryError_;
            return false;
        }
        bool ok = true;
        const auto load = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(SDL_LoadFunction(library_, name));
            ok = ok && fn != nullptr;
        };
        load(api_.initLibrary, "FPDF_InitLibrary");
        load(api_.destroyLibrary, "FPDF_DestroyLibrary");
        load(api_.loadCustomDocument, "FPDF_LoadCustomDocument");
        load(api_.getLastError, "FPDF_GetLastError");
        load(api_.getPageCount, "FPDF_GetPageCount");
        load(api_.getPageSizeByIndex, "FPDF_GetPageSizeByIndex");
        load(api_.loadPage, "FPDF_LoadPage");
        load(api_.closePage, "FPDF_ClosePage");
        load(api_.closeDocument, "FPDF_CloseDocument");
        load(api_.bitmapCreateEx, "FPDFBitmap_CreateEx");
        load(api_.bitmapFillRect, "FPDFBitmap_FillRect");
        load(api_.bitmapGetBuffer, "FPDFBitmap_GetBuffer");
        load(api_.bitmapGetStride, "FPDFBitmap_GetStride");
        load(api_.bitmapDestroy, "FPDFBitmap_Destroy");
        load(api_.renderPageBitmap, "FPDF_RenderPageBitmap");
        if (!ok) {
            libraryError_ = std::string(kLibraryName) + " is missing functions AtlasWriter needs (an old version?).";
            error = libraryError_;
            return false;
        }
        api_.initLibrary();
        initialized_ = true;
        return true;
    }

    static int readBlock(void* param, unsigned long position, unsigned char* buffer, unsigned long size)
    {
        SDL_IOStream* io = static_cast<State*>(param)->io;
        if (SDL_SeekIO(io, static_cast<Sint64>(position), SDL_IO_SEEK_SET) < 0)
            return 0;
        return SDL_ReadIO(io, buffer, size) == size ? 1 : 0;
    }

    void open(State& state)
    {
        std::string error;
        std::vector<PageSize> pages;
        if (loadLibrary(error)) {
            state.io = SDL_IOFromFile(state.path.c_str(), "rb");
            if (!state.io) {
                error = std::string("The file could not be opened:\n") + SDL_GetError();
            } else {
                const Sint64 size = SDL_GetIOSize(state.io);
                state.access = {static_cast<unsigned long>(std::max<Sint64>(0, size)), &Worker::readBlock, &state};
                state.document = api_.loadCustomDocument(&state.access, nullptr);
                if (!state.document) {
                    error = describeError(api_.getLastError());
                } else {
                    const int count = api_.getPageCount(state.document);
                    for (int i = 0; i < count; ++i) {
                        double w = 0.0, h = 0.0;
                        if (!api_.getPageSizeByIndex(state.document, i, &w, &h) || w <= 0.0 || h <= 0.0)
                            w = 612.0, h = 792.0; // US Letter, for a page PDFium can't size
                        pages.push_back({static_cast<float>(w), static_cast<float>(h)});
                    }
                    if (pages.empty())
                        error = "This PDF has no pages.";
                }
            }
        }
        if (!error.empty())
            close(state);
        else
            open_.push_back(current_);
        {
            std::lock_guard lock(state.mutex);
            state.status = error.empty() ? Document::Status::Ready : Document::Status::Failed;
            state.error = std::move(error);
            state.pages = std::move(pages);
        }
        notify(state);
    }

    void close(State& state)
    {
        if (state.document) {
            api_.closeDocument(state.document);
            state.document = nullptr;
        }
        if (state.io) {
            SDL_CloseIO(state.io);
            state.io = nullptr;
        }
    }

    Bitmap render(State& state, int page, int width)
    {
        Bitmap out;
        out.page = page;
        const PageSize size = state.pages[static_cast<size_t>(page)];
        width = std::clamp(width, 1, 8192);
        const int height = std::clamp(static_cast<int>(std::lround(width * size.height / size.width)), 1, 16384);
        FPDF_PAGE handle = api_.loadPage(state.document, page);
        FPDF_BITMAP bitmap = handle ? api_.bitmapCreateEx(width, height, kFormatBGRA, nullptr, 0) : nullptr;
        if (bitmap) {
            api_.bitmapFillRect(bitmap, 0, 0, width, height, 0xFFFFFFFFul);
            api_.renderPageBitmap(bitmap, handle, 0, 0, width, height, 0, kRenderAnnotations);
            const auto* src = static_cast<const uint8_t*>(api_.bitmapGetBuffer(bitmap));
            const int stride = api_.bitmapGetStride(bitmap);
            out.width = width;
            out.height = height;
            out.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
            for (int y = 0; y < height; ++y)
                std::copy_n(src + static_cast<ptrdiff_t>(y) * stride, static_cast<size_t>(width) * 4,
                            out.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4);
            api_.bitmapDestroy(bitmap);
        }
        if (handle)
            api_.closePage(handle);
        return out;
    }

    void process(const std::shared_ptr<State>& shared)
    {
        State& state = *shared;
        current_ = shared;
        bool closed = false;
        Document::Status status{};
        {
            std::lock_guard lock(state.mutex);
            closed = state.closed;
            status = state.status;
        }
        if (closed) {
            close(state);
            open_.erase(std::remove(open_.begin(), open_.end(), shared), open_.end());
            current_.reset();
            return;
        }
        if (status == Document::Status::Loading && !state.document)
            open(state);

        while (state.document) {
            std::pair<int, int> job;
            {
                std::lock_guard lock(state.mutex);
                if (state.closed || state.wanted.empty())
                    break;
                job = state.wanted.front();
                state.wanted.erase(state.wanted.begin());
            }
            if (job.first < 0 || static_cast<size_t>(job.first) >= state.pages.size())
                continue;
            Bitmap bitmap = render(state, job.first, job.second);
            {
                std::lock_guard lock(state.mutex);
                state.done.push_back(std::move(bitmap));
            }
            notify(state);
            std::lock_guard lock(mutex_);
            if (stopping_)
                break;
        }
        current_.reset();
    }

    static void notify(const State& state)
    {
        SDL_Event event{};
        event.type = state.event;
        SDL_PushEvent(&event);
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::shared_ptr<State>> queue_;
    std::thread thread_;
    bool stopping_ = false;

    // Worker thread only (and stop(), after the thread has ended)
    SDL_SharedObject* library_ = nullptr;
    bool initialized_ = false;
    std::string libraryError_;
    Api api_{};
    std::vector<std::shared_ptr<State>> open_; // documents PDFium holds
    std::shared_ptr<State> current_;
};

Worker& worker()
{
    static Worker instance;
    return instance;
}

} // namespace

Document::Document(const std::string& path, Uint32 event)
    : state_(std::make_shared<State>(path, event))
{
    worker().post(state_);
}

Document::~Document()
{
    {
        std::lock_guard lock(state_->mutex);
        state_->closed = true;
        state_->wanted.clear();
    }
    worker().post(state_);
}

Document::Status Document::status() const
{
    std::lock_guard lock(state_->mutex);
    return state_->status;
}

std::string Document::error() const
{
    std::lock_guard lock(state_->mutex);
    return state_->error;
}

const std::vector<PageSize>& Document::pages() const
{
    if (pages_.empty()) {
        std::lock_guard lock(state_->mutex);
        if (state_->status == Status::Ready)
            pages_ = state_->pages;
    }
    return pages_;
}

void Document::want(std::vector<std::pair<int, int>> pagesAndWidths)
{
    {
        std::lock_guard lock(state_->mutex);
        state_->wanted = std::move(pagesAndWidths);
    }
    worker().post(state_);
}

std::vector<Bitmap> Document::takeRendered()
{
    std::lock_guard lock(state_->mutex);
    return std::exchange(state_->done, {});
}

void shutdown()
{
    worker().stop();
}

} // namespace pdf
