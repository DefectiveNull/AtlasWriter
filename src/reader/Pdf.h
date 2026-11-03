#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// PDF files through PDFium, which is loaded at run time (pdfium.dll or
// libpdfium.so next to the executable): without it, PDFs show an error
// and everything else works. PDFium isn't thread-safe, so one worker
// thread makes every call into it; documents talk to it through queues.
namespace pdf {

struct PageSize
{
    float width, height; // points (1/72 inch)
};

// A rendered page: BGRA pixels, top row first.
struct Bitmap
{
    int page = 0;
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;
};

struct State; // shared with the worker

class Document
{
public:
    // Starts opening `path` on the worker; `event` is pushed whenever there
    // is news (opened, failed, a page rendered).
    Document(const std::string& path, Uint32 event);
    ~Document(); // closes it on the worker; pending renders are dropped

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    enum class Status { Loading, Ready, Failed };
    Status status() const;
    std::string error() const;
    const std::vector<PageSize>& pages() const; // valid once Ready

    // The pages to render, each at a pixel width, most wanted first.
    // Replaces the previous list; pages already being drawn still arrive.
    void want(std::vector<std::pair<int, int>> pagesAndWidths);
    // Rendered pages since the last call.
    std::vector<Bitmap> takeRendered();

private:
    std::shared_ptr<State> state_;
    mutable std::vector<PageSize> pages_; // copied from the state once ready
};

// Stops the worker and unloads PDFium (at exit, after every Document is gone).
void shutdown();

} // namespace pdf
