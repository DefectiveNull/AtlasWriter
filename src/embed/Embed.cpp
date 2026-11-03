#include "Embed.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

namespace embed {

namespace {

constexpr std::string_view kAssets = "assets/";

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

bool startsWithNoCase(std::string_view s, std::string_view prefix)
{
    if (s.size() < prefix.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (SDL_tolower(static_cast<unsigned char>(s[i])) != SDL_tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

// Separators as '/', and lower case where the file system ignores case.
std::string comparable(std::string_view path)
{
    std::string out(path);
    for (char& c : out) {
        if (c == '\\')
            c = '/';
#if defined(SDL_PLATFORM_WINDOWS)
        c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
#endif
    }
    return out;
}

bool isFile(const std::string& path)
{
    SDL_PathInfo info{};
    return SDL_GetPathInfo(path.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}

std::string folderOf(std::string_view path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash + 1));
}

} // namespace

bool isUrl(std::string_view text)
{
    text = trim(text);
    size_t scheme = 0;
    if (startsWithNoCase(text, "https://"))
        scheme = 8;
    else if (startsWithNoCase(text, "http://"))
        scheme = 7;
    if (scheme == 0 || text.size() <= scheme)
        return false;
    for (const char c : text) {
        if (static_cast<unsigned char>(c) <= ' ')
            return false;
    }
    return true;
}

std::string urlLabel(std::string_view url)
{
    url = trim(url);
    if (startsWithNoCase(url, "https://"))
        url.remove_prefix(8);
    else if (startsWithNoCase(url, "http://"))
        url.remove_prefix(7);
    if (startsWithNoCase(url, "www."))
        url.remove_prefix(4);
    while (url.size() > 1 && url.back() == '/')
        url.remove_suffix(1);
    return std::string(url);
}

std::string urlLink(std::string_view url)
{
    url = trim(url);
    std::string label = urlLabel(url);
    // Brackets in the label would end it early.
    std::string escaped;
    for (const char c : label) {
        if (c == '[' || c == ']' || c == '\\')
            escaped += '\\';
        escaped += c;
    }
    std::string target(url);
    std::string safe;
    for (const char c : target) {
        if (c == '(')
            safe += "%28";
        else if (c == ')')
            safe += "%29";
        else
            safe += c;
    }
    return "[" + escaped + "](" + safe + ")";
}

bool isImage(std::string_view path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || path.find_first_of("/\\", dot) != std::string_view::npos)
        return false;
    std::string ext(path.substr(dot + 1));
    for (char& c : ext)
        c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
    static constexpr std::string_view kImages[] = {"png", "jpg", "jpeg", "gif", "bmp", "tga", "svg", "webp"};
    return std::find(std::begin(kImages), std::end(kImages), ext) != std::end(kImages);
}

std::string fileName(std::string_view path)
{
    const size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string encodeLinkPath(std::string_view path)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : path) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '\\') {
            out += '/';
        } else if (u <= ' ' || c == '(' || c == ')' || c == '%' || c == '<' || c == '>' || c == '[' || c == ']'
                   || c == '#' || c == '?' || c == '"' || u == 0x7F) {
            out += '%';
            out += kHex[u >> 4];
            out += kHex[u & 15];
        } else {
            out += c; // UTF-8 bytes stay as they are
        }
    }
    return out;
}

std::string decodeLinkPath(std::string_view path)
{
    const auto hex = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;
    };
    std::string out;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '%' && i + 2 < path.size() && hex(path[i + 1]) >= 0 && hex(path[i + 2]) >= 0) {
            out += static_cast<char>(hex(path[i + 1]) * 16 + hex(path[i + 2]));
            i += 2;
        } else {
            out += path[i];
        }
    }
    return out;
}

std::string pathInside(std::string_view folder, std::string_view file)
{
    if (folder.empty())
        return {};
    std::string f = comparable(folder);
    if (f.back() != '/')
        f += '/';
    const std::string p = comparable(file);
    if (p.size() <= f.size() || p.compare(0, f.size(), f) != 0)
        return {};
    std::string rel(file.substr(f.size()));
    std::replace(rel.begin(), rel.end(), '\\', '/');
    return rel;
}

bool sameContents(const std::string& a, const std::string& b)
{
    SDL_PathInfo ia{}, ib{};
    if (!SDL_GetPathInfo(a.c_str(), &ia) || !SDL_GetPathInfo(b.c_str(), &ib) || ia.size != ib.size)
        return false;
    SDL_IOStream* fa = SDL_IOFromFile(a.c_str(), "rb");
    SDL_IOStream* fb = SDL_IOFromFile(b.c_str(), "rb");
    bool same = fa && fb;
    std::vector<char> ba(64 * 1024), bb(64 * 1024);
    while (same) {
        const size_t na = SDL_ReadIO(fa, ba.data(), ba.size());
        const size_t nb = SDL_ReadIO(fb, bb.data(), bb.size());
        if (na != nb || std::memcmp(ba.data(), bb.data(), na) != 0)
            same = false;
        else if (na == 0)
            break;
    }
    if (fa)
        SDL_CloseIO(fa);
    if (fb)
        SDL_CloseIO(fb);
    return same;
}

std::string assetName(const std::string& assetsDir, const std::string& file)
{
    const std::string name = fileName(file);
    const size_t dot = name.find_last_of('.');
    const std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    const std::string ext = dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot);
    for (int n = 0;; ++n) {
        const std::string candidate = n == 0 ? name : stem + "-" + std::to_string(n) + ext;
        const std::string path = assetsDir + candidate;
        SDL_PathInfo info{};
        if (!SDL_GetPathInfo(path.c_str(), &info))
            return candidate; // free
        if (info.type == SDL_PATHTYPE_FILE && sameContents(path, file))
            return candidate; // already there
    }
}

bool makeFolders(const std::string& folder)
{
    SDL_PathInfo info{};
    if (SDL_GetPathInfo(folder.c_str(), &info))
        return info.type == SDL_PATHTYPE_DIRECTORY;
    std::string path = folder;
    while (!path.empty() && (path.back() == '/' || path.back() == '\\'))
        path.pop_back();
    const size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos && slash > 0 && !makeFolders(path.substr(0, slash + 1)))
        return false;
    return SDL_CreateDirectory(path.c_str());
}

bool placeFile(const std::string& file, const std::string& base, std::string& target, std::string& error)
{
    if (const std::string rel = pathInside(base, file); !rel.empty()) {
        target = encodeLinkPath(rel);
        return true;
    }
    const std::string assetsDir = base + std::string(kAssets);
    if (!makeFolders(assetsDir)) {
        error = "Could not create " + assetsDir + ": " + SDL_GetError();
        return false;
    }
    const std::string name = assetName(assetsDir, file);
    const std::string destination = assetsDir + name;
    if (!isFile(destination) && !SDL_CopyFile(file.c_str(), destination.c_str())) {
        error = "Could not copy \"" + file + "\" to " + assetsDir + ": " + SDL_GetError();
        return false;
    }
    target = std::string(kAssets) + encodeLinkPath(name);
    return true;
}

std::string fileLink(std::string_view name, std::string_view target, bool image)
{
    std::string label;
    for (const char c : name) {
        if (c == '[' || c == ']' || c == '\\')
            label += '\\';
        label += c;
    }
    return std::string(image ? "![" : "[") + label + "](" + std::string(target) + ")";
}

std::string asBlock(std::string_view before, std::string_view after, std::string_view markdown)
{
    std::string out;
    if (!before.empty() && !before.ends_with("\n\n"))
        out += before.back() == '\n' ? "\n" : "\n\n";
    out += markdown;
    if (after.empty())
        out += '\n';
    else if (!after.starts_with("\n\n"))
        out += after.front() == '\n' ? "\n" : "\n\n";
    return out;
}

std::vector<LinkRef> assetLinks(std::string_view text)
{
    std::vector<LinkRef> out;
    const auto take = [&](size_t start, std::string_view stops) {
        size_t end = start;
        while (end < text.size() && stops.find(text[end]) == std::string_view::npos)
            ++end;
        const std::string_view target = text.substr(start, end - start);
        if (target.starts_with(kAssets) && target.size() > kAssets.size())
            out.push_back({start, std::string(target)});
    };
    // ](target) and ](<target>)
    for (size_t i = text.find("]("); i != std::string_view::npos; i = text.find("](", i + 2)) {
        const size_t start = i + 2;
        if (start < text.size() && text[start] == '<')
            take(start + 1, ">\n");
        else
            take(start, ") \t\n");
    }
    // <img src="target">
    for (size_t i = text.find("src="); i != std::string_view::npos; i = text.find("src=", i + 4)) {
        const size_t quote = i + 4;
        if (quote < text.size() && (text[quote] == '"' || text[quote] == '\''))
            take(quote + 1, text[quote] == '"' ? "\"\n" : "'\n");
    }
    std::sort(out.begin(), out.end(), [](const LinkRef& a, const LinkRef& b) { return a.pos < b.pos; });
    return out;
}

std::vector<Rename> transferAssets(std::string_view text, const std::string& oldBase, const std::string& newBase)
{
    std::vector<Rename> renames;
    if (oldBase.empty() || newBase.empty() || comparable(oldBase) == comparable(newBase))
        return renames;
    std::map<std::string, std::string> done; // link target -> target it ends up as
    for (const LinkRef& link : assetLinks(text)) {
        auto it = done.find(link.target);
        if (it == done.end()) {
            std::string result = link.target;
            const std::string rel = decodeLinkPath(link.target); // assets/...
            const std::string source = oldBase + rel;
            const std::string folder = newBase + folderOf(rel);
            if (isFile(source) && makeFolders(folder)) {
                const std::string name = assetName(folder, source);
                const std::string destination = folder + name;
                const bool there = isFile(destination) || SDL_CopyFile(source.c_str(), destination.c_str());
                if (there && name != fileName(rel))
                    result = encodeLinkPath(folderOf(rel) + name);
            }
            it = done.emplace(link.target, std::move(result)).first;
        }
        if (it->second != link.target)
            renames.push_back({link.pos, link.target.size(), it->second});
    }
    std::sort(renames.begin(), renames.end(), [](const Rename& a, const Rename& b) { return a.pos > b.pos; });
    return renames;
}

} // namespace embed
