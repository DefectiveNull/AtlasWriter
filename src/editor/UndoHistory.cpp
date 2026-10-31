#include "UndoHistory.h"

namespace {

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

} // namespace

bool UndoHistory::merges(const Group& group, Kind kind, const Edit& edit, uint64_t nowMs) const
{
    if (!mergeable_ || group.kind != kind || kind == Kind::Other || nowMs - group.time > kMergeMs)
        return false;
    const Edit& last = group.edits.back();

    switch (kind) {
    case Kind::Typing: {
        // Contiguous, and a new word starts a new group: "hello world"
        // undoes as "world" then "hello ".
        if (!edit.removed.empty() || edit.pos != last.pos + last.inserted.size() || last.inserted.empty())
            return false;
        const bool newWord = !isSpace(edit.inserted.front()) && isSpace(last.inserted.back());
        return !newWord && edit.inserted.find('\n') == std::string::npos;
    }
    case Kind::Backspace:
        return edit.inserted.empty() && last.inserted.empty() && edit.pos + edit.removed.size() == last.pos;
    case Kind::Delete:
        return edit.inserted.empty() && last.inserted.empty() && edit.pos == last.pos;
    case Kind::Other:
        break;
    }
    return false;
}

void UndoHistory::record(Kind kind, Edit edit, Selection before, Selection after, uint64_t nowMs)
{
    redo_.clear();
    if (!undo_.empty() && merges(undo_.back(), kind, edit, nowMs)) {
        Group& group = undo_.back();
        Edit& last = group.edits.back();
        // Fold into the previous edit so long runs of typing stay one Edit.
        if (kind == Kind::Typing) {
            last.inserted += edit.inserted;
        } else if (kind == Kind::Backspace) {
            last.removed.insert(0, edit.removed);
            last.pos = edit.pos;
        } else {
            last.removed += edit.removed;
        }
        group.after = after;
        group.time = nowMs;
        group.id = nextId_++; // a new document state
        mergeable_ = true;
        return;
    }

    Group group{{std::move(edit)}, before, after, kind, nowMs, nextId_++};
    undo_.push_back(std::move(group));
    if (undo_.size() > kMaxGroups)
        undo_.erase(undo_.begin());
    mergeable_ = true;
}

void UndoHistory::commitUndo()
{
    redo_.push_back(std::move(undo_.back()));
    undo_.pop_back();
    mergeable_ = false;
}

void UndoHistory::commitRedo()
{
    undo_.push_back(std::move(redo_.back()));
    redo_.pop_back();
    mergeable_ = false;
}

void UndoHistory::clear()
{
    undo_.clear();
    redo_.clear();
    mergeable_ = false;
}
