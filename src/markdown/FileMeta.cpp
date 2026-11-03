#include "FileMeta.h"

#if defined(__linux__)
#include <sys/xattr.h>

#include <string>

namespace {

void setOrRemove(const char* path, const char* name, const std::string& value)
{
    if (value.empty())
        removexattr(path, name); // ENODATA when it wasn't set: fine
    else
        setxattr(path, name, value.data(), value.size(), 0);
}

} // namespace

void writeFileMetadata(const char* path, const md::Meta& meta)
{
    std::string tags;
    for (const std::string& tag : meta.tags) {
        if (!tags.empty())
            tags += ',';
        tags += tag;
    }
    setOrRemove(path, "user.xdg.tags", tags);
    setOrRemove(path, "user.xdg.comment", meta.description);
    setOrRemove(path, "user.dublincore.title", meta.title);
    setOrRemove(path, "user.dublincore.creator", meta.author);
    setOrRemove(path, "user.dublincore.date", meta.shownDate());
}

#else

void writeFileMetadata(const char*, const md::Meta&)
{
}

#endif
