#include "Document.h"

#include "markdown/FileMeta.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace {

constexpr const char* kUtf8Bom = "\xEF\xBB\xBF";
constexpr size_t kMetaScanBytes = 64 * 1024; // the block sits at the top

} // namespace

bool samePath(std::string a, std::string b)
{
    for (std::string* s : {&a, &b}) {
        for (char& c : *s) {
            if (c == '\\')
                c = '/';
#if defined(SDL_PLATFORM_WINDOWS)
            c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
#endif
        }
    }
    return a == b;
}

bool Document::load(const std::string& path, std::string& error)
{
    size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (!data) {
        error = SDL_GetError();
        return false;
    }
    std::string_view raw(static_cast<const char*>(data), size);
    bom_ = raw.starts_with(kUtf8Bom);
    if (bom_)
        raw.remove_prefix(3);
    bool hadCrlf = false;
    const std::string text = normalizeNewlines(raw, &hadCrlf);
    SDL_free(data);

    buffer_.setText(text);
    crlf_ = hadCrlf;
    path_ = path;
    type_ = detectFileType(path_);
    history_.clear();
    savedState_ = history_.stateId();
    metaVersion_ = UINT64_MAX;
    for (DocumentListener* listener : listeners_)
        listener->documentReset();
    return true;
}

bool Document::save(const std::string& path, std::string& error)
{
    const std::string text = buffer_.toString();
    std::string out;
    out.reserve(text.size() + (crlf_ ? buffer_.lineCount() : 0) + 3);
    if (bom_)
        out += kUtf8Bom;
    if (crlf_) {
        for (const char c : text) {
            if (c == '\n')
                out += '\r';
            out += c;
        }
    } else {
        out += text;
    }
    if (!SDL_SaveFile(path.c_str(), out.data(), out.size())) {
        error = SDL_GetError();
        return false;
    }
    path_ = path;
    type_ = detectFileType(path_); // saving as notes.md makes it Markdown
    if (type_.kind == FileKind::Markdown)
        writeFileMetadata(path.c_str(), md::parseMeta(std::string_view(text).substr(0, kMetaScanBytes)));
    savedState_ = history_.stateId();
    history_.breakGroup(); // so undo can land exactly on the saved text
    metaVersion_ = UINT64_MAX;
    return true;
}

void Document::applyEdit(size_t pos, size_t length, std::string_view text)
{
    DocumentEdit edit{};
    edit.pos = pos;
    edit.removed = length;
    edit.inserted = text.size();
    edit.versionBefore = buffer_.version();
    edit.firstLine = buffer_.lineOf(pos);
    const size_t oldLast = buffer_.lineOf(pos + length);

    if (length > 0)
        buffer_.erase(pos, length);
    if (!text.empty())
        buffer_.insert(pos, text);

    edit.oldLines = oldLast - edit.firstLine + 1;
    edit.newLines = buffer_.lineOf(pos + text.size()) - edit.firstLine + 1;
    for (DocumentListener* listener : listeners_)
        listener->documentEdited(edit);
}

void Document::edit(size_t pos, size_t length, std::string_view text)
{
    std::string removed = buffer_.substr(pos, length);
    applyEdit(pos, length, text);
    const UndoHistory::Selection caret{pos + text.size(), pos + text.size()};
    history_.breakGroup();
    history_.record(UndoHistory::Kind::Other, {pos, std::move(removed), std::string(text)}, caret, caret,
                    SDL_GetTicks());
    history_.breakGroup();
}

void Document::addListener(DocumentListener* listener)
{
    listeners_.push_back(listener);
}

void Document::removeListener(DocumentListener* listener)
{
    std::erase(listeners_, listener);
}

std::string Document::displayName() const
{
    if (path_.empty())
        return "Untitled";
    const size_t slash = path_.find_last_of("/\\");
    return slash == std::string::npos ? path_ : path_.substr(slash + 1);
}

std::string Document::directory() const
{
    const size_t slash = path_.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path_.substr(0, slash + 1);
}

std::string Document::title() const
{
    // meta_ may be a little stale here (const); the frame's meta() call refreshes it.
    return meta_.title.empty() ? displayName() : meta_.title;
}

const md::Meta& Document::meta()
{
    readMeta();
    return meta_;
}

const std::string& Document::metaSummary()
{
    readMeta();
    return metaSummary_;
}

void Document::readMeta()
{
    if (metaVersion_ == buffer_.version() && metaKind_ == type_.kind)
        return;
    metaVersion_ = buffer_.version();
    metaKind_ = type_.kind;
    meta_ = {};
    metaSummary_.clear();
    if (type_.kind != FileKind::Markdown)
        return;

    meta_ = md::parseMeta(buffer_.substr(0, std::min(buffer_.size(), kMetaScanBytes)));
    if (!meta_.error.empty()) {
        metaSummary_ = "meta: " + meta_.error;
        return;
    }
    const auto append = [&](std::string_view part) {
        if (part.empty())
            return;
        if (!metaSummary_.empty())
            metaSummary_ += " \xC2\xB7 "; // middle dot
        metaSummary_ += part;
    };
    std::string tags;
    for (const std::string& tag : meta_.tags)
        tags += (tags.empty() ? "#" : " #") + tag;
    append(tags);
    append(meta_.shownDate());
    append(meta_.author);
    if (metaSummary_.size() > 100) {
        size_t cut = 97;
        while (cut > 0 && (static_cast<unsigned char>(metaSummary_[cut]) & 0xC0) == 0x80)
            --cut; // don't split a UTF-8 sequence
        metaSummary_.resize(cut);
        metaSummary_ += "...";
    }
}
