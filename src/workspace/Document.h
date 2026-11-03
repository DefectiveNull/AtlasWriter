#pragma once

#include "editor/FileType.h"
#include "editor/TextBuffer.h"
#include "editor/UndoHistory.h"
#include "markdown/Meta.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// One change to a document's text, as every view of it sees it.
struct DocumentEdit
{
    size_t pos;
    size_t removed;     // bytes removed at pos
    size_t inserted;    // bytes inserted at pos
    size_t firstLine;   // lines [firstLine, firstLine + oldLines) became
    size_t oldLines;    //   [firstLine, firstLine + newLines)
    size_t newLines;
    uint64_t versionBefore; // buffer version just before the edit
};

// Something showing a document (an EditorView). Told about every change,
// whichever view made it, so two tiles on one file stay in sync.
class DocumentListener
{
public:
    virtual void documentEdited(const DocumentEdit& edit) = 0;
    virtual void documentReset() = 0; // the whole text was replaced
protected:
    ~DocumentListener() = default;
};

// A file (or an unsaved new document): its text, how it was encoded on
// disk, and its undo history. Tabs share Documents, so the same file in two
// tiles is one Document with two views.
class Document
{
public:
    Document() = default;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // Loads `path` (BOM and CRLF are remembered and restored on save).
    // On failure returns false with the reason in `error`, leaving the
    // document as it was.
    bool load(const std::string& path, std::string& error);
    // Writes the text to `path`, which becomes the document's path.
    bool save(const std::string& path, std::string& error);

    const TextBuffer& buffer() const { return buffer_; }
    UndoHistory& history() { return history_; }
    const UndoHistory& history() const { return history_; }

    // The only way the text changes, so every listener hears about it.
    void applyEdit(size_t pos, size_t length, std::string_view text);
    // An edit made outside any editor view (a checkbox clicked in a preview
    // tile): applied and recorded as an undo step of its own.
    void edit(size_t pos, size_t length, std::string_view text);

    void addListener(DocumentListener* listener);
    void removeListener(DocumentListener* listener);

    const std::string& path() const { return path_; }
    const FileType& type() const { return type_; }
    bool isMarkdown() const { return type_.kind == FileKind::Markdown; }
    bool crlf() const { return crlf_; }
    bool bom() const { return bom_; }

    bool modified() const { return history_.stateId() != savedState_; }
    // An untitled document nobody has typed in: opening a file replaces it.
    bool pristine() const { return path_.empty() && buffer_.size() == 0 && !history_.canUndo(); }
    std::string displayName() const; // file name, or "Untitled"
    std::string directory() const;   // folder with a trailing separator; empty if untitled
    std::string title() const;       // the meta block's title, else displayName()

    // The ```meta block at the top of a Markdown file, re-read after edits.
    const md::Meta& meta();
    const std::string& metaSummary(); // for the status bar: "#tag #tag · date · author"

private:
    void readMeta();

    TextBuffer buffer_;
    UndoHistory history_;
    std::vector<DocumentListener*> listeners_;

    std::string path_; // empty for an unsaved new document
    FileType type_;
    uint64_t savedState_ = 0; // history state at the last save
    bool crlf_ = false;
    bool bom_ = false;

    md::Meta meta_;
    std::string metaSummary_;
    uint64_t metaVersion_ = UINT64_MAX;
    FileKind metaKind_ = FileKind::Code;
};

// Paths compared the way the file system does (case-insensitive on Windows).
bool samePath(std::string a, std::string b);
