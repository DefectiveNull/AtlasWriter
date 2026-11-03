#include "SessionAssets.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace session {

namespace {

// <temp>/AtlasWriter/, with a trailing separator.
const std::string& root()
{
    static const std::string path = [] {
        const char* temp = nullptr;
#if defined(SDL_PLATFORM_WINDOWS)
        temp = SDL_getenv("TEMP");
        if (!temp || !*temp)
            temp = SDL_getenv("TMP");
#else
        temp = SDL_getenv("TMPDIR");
        if (!temp || !*temp)
            temp = "/tmp";
#endif
        std::string dir = temp && *temp ? temp : ".";
        if (dir.back() != '/' && dir.back() != '\\')
            dir += '/';
        return dir + "AtlasWriter/";
    }();
    return path;
}

// This run's folder: named after the time it started, which also dates it
// for sweep().
const std::string& current()
{
    static const std::string path = [] {
        SDL_Time now = 0;
        SDL_GetCurrentTime(&now);
        return root() + "run-" + std::to_string(now) + "/";
    }();
    return path;
}

// Deletes `path` and everything in it; only ever inside root().
void removeTree(const std::string& path)
{
    const auto slashes = [](std::string s) {
        for (char& c : s) {
            if (c == '\\')
                c = '/';
        }
        return s;
    };
    const std::string p = slashes(path), r = slashes(root());
    if (p.size() <= r.size() || p.compare(0, r.size(), r) != 0 || p.find("/../") != std::string::npos)
        return;
    SDL_PathInfo info{};
    if (!SDL_GetPathInfo(path.c_str(), &info))
        return;
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        std::vector<std::string> children;
        SDL_EnumerateDirectory(
            path.c_str(),
            [](void* userdata, const char* dirname, const char* fname) -> SDL_EnumerationResult {
                static_cast<std::vector<std::string>*>(userdata)->push_back(std::string(dirname) + fname);
                return SDL_ENUM_CONTINUE;
            },
            &children);
        for (const std::string& child : children)
            removeTree(child);
    }
    SDL_RemovePath(path.c_str());
}

} // namespace

std::string folderFor(uint64_t id)
{
    return current() + "doc-" + std::to_string(id) + "/";
}

std::string folderNamed(const std::string& name)
{
    return current() + name + "/";
}

void cleanup()
{
    std::string path = current();
    path.pop_back(); // no trailing separator for the folder itself
    removeTree(path);
}

void remove(const std::string& folder)
{
    std::string path = folder;
    while (!path.empty() && (path.back() == '/' || path.back() == '\\'))
        path.pop_back();
    removeTree(path); // refuses anything outside root()
}

void sweep(int days)
{
    SDL_Time now = 0;
    if (!SDL_GetCurrentTime(&now))
        return;
    const SDL_Time cutoff = now - static_cast<SDL_Time>(days) * 24 * 3600 * SDL_NS_PER_SECOND;
    std::vector<std::string> old;
    struct Context
    {
        SDL_Time cutoff;
        std::vector<std::string>* old;
    } context{cutoff, &old};
    SDL_EnumerateDirectory(
        root().c_str(),
        [](void* userdata, const char* dirname, const char* fname) -> SDL_EnumerationResult {
            auto* c = static_cast<Context*>(userdata);
            const std::string name = fname;
            if (name.rfind("run-", 0) == 0) {
                const SDL_Time started = SDL_strtoll(name.c_str() + 4, nullptr, 10);
                if (started > 0 && started < c->cutoff)
                    c->old->push_back(std::string(dirname) + name);
            }
            return SDL_ENUM_CONTINUE;
        },
        &context);
    for (const std::string& path : old)
        removeTree(path);
}

} // namespace session
