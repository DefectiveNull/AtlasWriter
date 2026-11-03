#pragma once

#include "markdown/Meta.h"

// Copies a Markdown file's metadata to where file managers look for it.
//
// Linux: extended attributes under the freedesktop names, which KDE's Dolphin
// shows and indexes (user.xdg.tags, user.xdg.comment, user.dublincore.title,
// user.dublincore.creator, user.dublincore.date). Fields the meta block
// doesn't set are removed, so deleting a tag in the editor removes it there.
// Filesystems without xattrs (FAT, some network mounts) are skipped quietly.
//
// Windows: nothing to do here; Explorer reads the ```meta block itself
// through AtlasWriterProps.dll (see src/explorer).
void writeFileMetadata(const char* path, const md::Meta& meta);
