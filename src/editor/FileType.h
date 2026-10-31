#pragma once

#include <string_view>

// Which workspace a document opens in.
enum class FileKind { Text, Markdown, Code };

struct FileType
{
    FileKind kind = FileKind::Text;
    const char* name = "Plain Text"; // shown in the status bar
};

// Detects the file type from a path's file name and extension
// (case-insensitive). Unknown files are plain text.
FileType detectFileType(std::string_view path);

// Semicolon-separated extension lists for file dialog filters,
// e.g. "md;markdown". Built once, valid for the program's lifetime.
const char* markdownExtensions();
const char* codeExtensions();
const char* supportedExtensions(); // text + markdown + code
