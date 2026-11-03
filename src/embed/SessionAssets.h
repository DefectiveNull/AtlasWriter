#pragma once

#include <cstdint>
#include <string>

// Where an unsaved document keeps the files dropped into it until it is
// saved: <temp>/AtlasWriter/<session>/<document>/, so its links can already
// be written as assets/... (see embed::transferAssets for the move on save).
namespace session {

// The folder for document `id`, with a trailing separator. Not created
// until something is copied into it.
std::string folderFor(uint64_t id);
// Deletes this run's folder (at exit).
void cleanup();
// Deletes one document's folder (after its first save copied the files
// out). Does nothing for paths outside the session folders.
void remove(const std::string& folder);
// Deletes folders left by runs that ended more than `days` ago, e.g. after
// a crash (at startup).
void sweep(int days = 7);

} // namespace session
