#pragma once

#include <string>
#include <vector>

// An EPUB book unpacked into a folder, so chapters are HTML files and their
// images load like any other file.
namespace epub {

struct Book
{
    std::string title;                 // from the package metadata; may be empty
    std::vector<std::string> chapters; // absolute paths, in reading order
};

// Unpacks `path` into `folder` (created as needed; trailing separator) and
// reads its reading order. Entries with unsafe names (absolute, "..") are
// skipped.
bool load(const std::string& path, const std::string& folder, Book& book, std::string& error);

// "a/b/../c.xhtml" -> "a/c.xhtml"; '\' becomes '/'.
std::string normalizePath(std::string path);

} // namespace epub
