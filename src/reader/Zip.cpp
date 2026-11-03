#include "reader/Zip.h"

#include <SDL3/SDL.h>
#include <stb_image.h> // its zlib decoder inflates deflated entries

namespace {

uint16_t u16(const std::string& d, size_t at)
{
    return static_cast<uint16_t>(static_cast<uint8_t>(d[at]) | static_cast<uint8_t>(d[at + 1]) << 8);
}

uint32_t u32(const std::string& d, size_t at)
{
    return static_cast<uint32_t>(u16(d, at)) | static_cast<uint32_t>(u16(d, at + 2)) << 16;
}

constexpr uint32_t kEndSignature = 0x06054b50;
constexpr uint32_t kCentralSignature = 0x02014b50;
constexpr uint32_t kLocalSignature = 0x04034b50;

} // namespace

bool Zip::open(const std::string& path, std::string& error)
{
    entries_.clear();
    size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (!data) {
        error = std::string("The file could not be read:\n") + SDL_GetError();
        return false;
    }
    data_.assign(static_cast<const char*>(data), size);
    SDL_free(data);

    // The end record is in the last 22 bytes plus up to 64 KB of comment.
    constexpr size_t kEndSize = 22;
    if (data_.size() < kEndSize) {
        error = "This is not a zip archive.";
        return false;
    }
    size_t end = std::string::npos;
    const size_t lowest = data_.size() > kEndSize + 0xFFFF ? data_.size() - kEndSize - 0xFFFF : 0;
    for (size_t at = data_.size() - kEndSize + 1; at-- > lowest;) {
        if (u32(data_, at) == kEndSignature) {
            end = at;
            break;
        }
    }
    if (end == std::string::npos) {
        error = "This is not a zip archive, or it is damaged.";
        return false;
    }
    const size_t count = u16(data_, end + 10);
    size_t at = u32(data_, end + 16);
    for (size_t i = 0; i < count; ++i) {
        if (at + 46 > data_.size() || u32(data_, at) != kCentralSignature) {
            error = "The archive's directory is damaged.";
            return false;
        }
        Entry entry;
        entry.method = u16(data_, at + 10);
        entry.compressedSize = u32(data_, at + 20);
        entry.size = u32(data_, at + 24);
        const size_t nameLength = u16(data_, at + 28);
        const size_t extraLength = u16(data_, at + 30);
        const size_t commentLength = u16(data_, at + 32);
        entry.localHeader = u32(data_, at + 42);
        if (at + 46 + nameLength > data_.size()) {
            error = "The archive's directory is damaged.";
            return false;
        }
        entry.name = data_.substr(at + 46, nameLength);
        for (char& c : entry.name) { // some Windows tools write "\"
            if (c == '\\')
                c = '/';
        }
        entries_.push_back(std::move(entry));
        at += 46 + nameLength + extraLength + commentLength;
    }
    return true;
}

const Zip::Entry* Zip::find(std::string_view name) const
{
    for (const Entry& entry : entries_) {
        if (entry.name == name)
            return &entry;
    }
    // Names in books' manifests sometimes differ from the archive in case.
    for (const Entry& entry : entries_) {
        if (entry.name.size() == name.size() && SDL_strncasecmp(entry.name.c_str(), name.data(), name.size()) == 0)
            return &entry;
    }
    return nullptr;
}

bool Zip::read(const Entry& entry, std::string& out) const
{
    const size_t at = entry.localHeader;
    if (at + 30 > data_.size() || u32(data_, at) != kLocalSignature)
        return false;
    const size_t start = at + 30 + u16(data_, at + 26) + u16(data_, at + 28);
    if (start + entry.compressedSize > data_.size())
        return false;
    const char* in = data_.data() + start;
    if (entry.method == 0) {
        out.assign(in, entry.compressedSize);
        return true;
    }
    if (entry.method != 8 || entry.size > 512u * 1024u * 1024u)
        return false;
    out.resize(entry.size);
    if (entry.size == 0)
        return true;
    const int n = stbi_zlib_decode_noheader_buffer(out.data(), static_cast<int>(out.size()), in,
                                                   static_cast<int>(entry.compressedSize));
    if (n < 0)
        return false;
    out.resize(static_cast<size_t>(n));
    return true;
}
