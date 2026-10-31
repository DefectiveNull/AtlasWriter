#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Undo/redo as a stack of groups of reversible edits.
//
// Each edit stores the text it removed and inserted at one position, so it
// can be applied in either direction without snapshots. Consecutive typing
// (or backspacing) merges into one group per word, so undo steps feel like
// a word processor's rather than one per keystroke.
//
// stateId() names the document state: it changes with every new edit, undo
// and redo, and returns to an earlier value when undo/redo returns to that
// state, so "is this the saved state?" is a simple comparison.
class UndoHistory
{
public:
    // What kind of edit, for merging consecutive ones into a group.
    enum class Kind { Typing, Backspace, Delete, Other };

    struct Edit
    {
        size_t pos;
        std::string removed;
        std::string inserted;
    };

    struct Selection
    {
        size_t cursor;
        size_t anchor;
    };

    struct Group
    {
        std::vector<Edit> edits;
        Selection before; // selection to restore on undo
        Selection after;  // ... and on redo
        Kind kind;
        uint64_t time;    // ms of the last edit merged in
        uint64_t id;
    };

    static constexpr size_t kMaxGroups = 5000;
    static constexpr uint64_t kMergeMs = 1000;

    // Records an edit already applied to the buffer. `before`/`after` are the
    // selections around it.
    void record(Kind kind, Edit edit, Selection before, Selection after, uint64_t nowMs);

    // Stops the current group from absorbing further edits (e.g. the caret
    // moved), so the next edit starts a new undo step.
    void breakGroup() { mergeable_ = false; }

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }

    // The group to revert / reapply; the caller applies its edits, then
    // calls the matching commit.
    const Group& peekUndo() const { return undo_.back(); }
    const Group& peekRedo() const { return redo_.back(); }
    void commitUndo();
    void commitRedo();

    void clear();
    uint64_t stateId() const { return undo_.empty() ? 0 : undo_.back().id; }

private:
    bool merges(const Group& group, Kind kind, const Edit& edit, uint64_t nowMs) const;

    std::vector<Group> undo_;
    std::vector<Group> redo_;
    uint64_t nextId_ = 1;
    bool mergeable_ = false;
};
