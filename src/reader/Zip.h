#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Reads zip archives (EPUB books): the central directory, and entries that
// are stored or deflated. No ZIP64, encryption or other methods, which
// books don't use. The whole archive is read into memory.
class Zip
{
public:
    struct Entry
    {
        std::string name; // as stored, '/'-separated
        uint16_t method = 0; // 0 stored, 8 deflate
        uint32_t compressedSize = 0;
        uint32_t size = 0;
        uint32_t localHeader = 0; // offset of the local header
    };

    bool open(const std::string& path, std::string& error);
    const std::vector<Entry>& entries() const { return entries_; }
    const Entry* find(std::string_view name) const;
    // An entry's contents; false if it is damaged or uses another method.
    bool read(const Entry& entry, std::string& out) const;

private:
    std::string data_;
    std::vector<Entry> entries_;
};
