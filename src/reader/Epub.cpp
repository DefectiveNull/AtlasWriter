#include "reader/Epub.h"

#include "embed/Embed.h"
#include "markdown/Html.h"
#include "reader/Zip.h"

#include <SDL3/SDL.h>

#include <map>
#include <string_view>

namespace epub {

namespace {

// One XML tag: its local name (lowercase, namespace prefix dropped) and the
// raw attribute text.
struct XmlTag
{
    std::string name;
    bool closing = false;
    std::string_view attributes;
    size_t end = 0; // just past '>'
};

// The next tag at or after `from`; false at the end.
bool nextTag(std::string_view xml, size_t from, XmlTag& tag)
{
    for (size_t at = xml.find('<', from); at != std::string_view::npos; at = xml.find('<', at + 1)) {
        if (xml.compare(at, 4, "<!--") == 0) {
            const size_t close = xml.find("-->", at);
            if (close == std::string_view::npos)
                return false;
            at = close;
            continue;
        }
        const size_t close = xml.find('>', at);
        if (close == std::string_view::npos)
            return false;
        size_t i = at + 1;
        tag = XmlTag{};
        if (i < close && xml[i] == '/') {
            tag.closing = true;
            ++i;
        }
        const size_t nameStart = i;
        while (i < close && xml[i] != ' ' && xml[i] != '\t' && xml[i] != '\r' && xml[i] != '\n' && xml[i] != '/')
            ++i;
        std::string_view name = xml.substr(nameStart, i - nameStart);
        if (const size_t colon = name.rfind(':'); colon != std::string_view::npos)
            name.remove_prefix(colon + 1);
        if (name.empty() || name[0] == '?' || name[0] == '!')
            continue;
        tag.name.assign(name);
        for (char& c : tag.name)
            c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
        tag.attributes = xml.substr(i, close - i);
        tag.end = close + 1;
        return true;
    }
    return false;
}

// The text after a tag up to the next '<', entities decoded and trimmed.
std::string textAfter(std::string_view xml, size_t from)
{
    const size_t end = std::min(xml.find('<', from), xml.size());
    std::string out;
    for (size_t i = from; i < end;) {
        if (xml[i] == '&') {
            if (const size_t n = md::decodeEntity(xml, i, out)) {
                i += n;
                continue;
            }
        }
        out += xml[i] == '\n' || xml[i] == '\r' || xml[i] == '\t' ? ' ' : xml[i];
        ++i;
    }
    const size_t first = out.find_first_not_of(' ');
    const size_t last = out.find_last_not_of(' ');
    return first == std::string::npos ? std::string() : out.substr(first, last - first + 1);
}

bool safeName(std::string_view name)
{
    if (name.empty() || name[0] == '/' || name[0] == '\\' || name.find(':') != std::string_view::npos)
        return false;
    size_t start = 0;
    while (start <= name.size()) {
        const size_t end = std::min(name.find_first_of("/\\", start), name.size());
        if (name.substr(start, end - start) == "..")
            return false;
        start = end + 1;
    }
    return true;
}

} // namespace

std::string normalizePath(std::string path)
{
    for (char& c : path) {
        if (c == '\\')
            c = '/';
    }
    std::vector<std::string> parts;
    size_t start = 0;
    const bool rooted = !path.empty() && path[0] == '/';
    while (start <= path.size()) {
        const size_t end = std::min(path.find('/', start), path.size());
        const std::string part = path.substr(start, end - start);
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..")
                parts.pop_back();
            else if (!rooted)
                parts.push_back(part);
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        start = end + 1;
    }
    std::string out = rooted ? "/" : "";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            out += '/';
        out += parts[i];
    }
    return out;
}

bool load(const std::string& path, const std::string& folder, Book& book, std::string& error)
{
    Zip zip;
    if (!zip.open(path, error))
        return false;

    // META-INF/container.xml names the package (.opf) file.
    std::string container;
    const Zip::Entry* entry = zip.find("META-INF/container.xml");
    if (!entry || !zip.read(*entry, container)) {
        error = "This is not an EPUB book (it has no META-INF/container.xml).";
        return false;
    }
    std::string opfName;
    XmlTag tag;
    for (size_t at = 0; nextTag(container, at, tag); at = tag.end) {
        if (tag.name == "rootfile" && !tag.closing) {
            opfName = md::attribute(tag.attributes, "full-path");
            break;
        }
    }
    std::string opf;
    entry = opfName.empty() ? nullptr : zip.find(opfName);
    if (!entry || !zip.read(*entry, opf)) {
        error = "The book's package file is missing or damaged.";
        return false;
    }
    const size_t slash = opfName.rfind('/');
    const std::string opfDir = slash == std::string::npos ? "" : opfName.substr(0, slash + 1);

    // Unpack everything: chapters, images, styles.
    if (!embed::makeFolders(folder)) {
        error = "Could not create a folder to unpack the book into:\n" + folder;
        return false;
    }
    for (const Zip::Entry& e : zip.entries()) {
        if (e.name.empty() || e.name.back() == '/' || !safeName(e.name))
            continue;
        std::string data;
        if (!zip.read(e, data))
            continue;
        const std::string target = folder + normalizePath(e.name);
        const size_t cut = target.find_last_of('/');
        if (cut != std::string::npos)
            embed::makeFolders(target.substr(0, cut + 1));
        SDL_SaveFile(target.c_str(), data.data(), data.size());
    }

    // The manifest maps ids to files; the spine lists them in reading order.
    std::map<std::string, std::string> manifest;
    std::vector<std::string> spine;
    bool inMetadata = false;
    for (size_t at = 0; nextTag(opf, at, tag); at = tag.end) {
        if (tag.name == "metadata") {
            inMetadata = !tag.closing;
        } else if (tag.name == "title" && !tag.closing && inMetadata && book.title.empty()) {
            book.title = textAfter(opf, tag.end);
        } else if (tag.name == "item" && !tag.closing) {
            const std::string type = md::attribute(tag.attributes, "media-type");
            const std::string href = md::attribute(tag.attributes, "href");
            const bool page = type == "application/xhtml+xml" || type == "text/html" || href.ends_with(".xhtml")
                           || href.ends_with(".html") || href.ends_with(".htm");
            if (page)
                manifest[md::attribute(tag.attributes, "id")] = href;
        } else if (tag.name == "itemref" && !tag.closing) {
            spine.push_back(md::attribute(tag.attributes, "idref"));
        }
    }
    for (const std::string& id : spine) {
        const auto it = manifest.find(id);
        if (it == manifest.end())
            continue;
        std::string href = it->second;
        href = href.substr(0, std::min(href.find('#'), href.size()));
        const std::string name = normalizePath(opfDir + embed::decodeLinkPath(href));
        if (safeName(name))
            book.chapters.push_back(folder + name);
    }
    if (book.chapters.empty()) {
        error = "The book has no chapters to show.";
        return false;
    }
    return true;
}

} // namespace epub
