#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Drag and drop and pasted links: turning dropped files and addresses into
// Markdown embeds, and keeping the files they point to beside the document.
//
// A file dropped from outside the document's folder is copied into an
// assets/ folder next to it and linked as assets/<name>; one already inside
// the folder is linked where it is. Images become ![name](path), anything
// else [name](path), which the rendered view draws as a card when it stands
// alone in its paragraph. Nothing here touches the UI.
namespace embed {

// A lone web address: http:// or https://, with no spaces or line breaks
// once surrounding whitespace is trimmed.
bool isUrl(std::string_view text);
// What a link to `url` shows: the address without its scheme, a leading
// "www." or a trailing "/" ("https://www.example.com/a/" -> "example.com/a").
std::string urlLabel(std::string_view url);
// [label](url), for a trimmed address.
std::string urlLink(std::string_view url);

// By extension: png, jpg, jpeg, gif, bmp, tga, svg, webp.
bool isImage(std::string_view path);
// The last path component.
std::string fileName(std::string_view path);
// A path written into a link: forward slashes; spaces, parentheses, '%'
// and other troublemakers percent-encoded. decodeLinkPath() reverses it.
std::string encodeLinkPath(std::string_view path);
std::string decodeLinkPath(std::string_view path);
// `file` relative to `folder` (which ends with a separator) when it is
// inside it, subfolders included; empty otherwise. Separators and, on
// Windows, letter case don't matter.
std::string pathInside(std::string_view folder, std::string_view file);

// Byte-for-byte comparison of two files.
bool sameContents(const std::string& a, const std::string& b);
// The name to give `file` in the folder `assetsDir` (with a trailing
// separator): its own name when that is free or already holds the same
// contents, else name-1.ext, name-2.ext, ...
std::string assetName(const std::string& assetsDir, const std::string& file);
// Creates `folder` and any missing parents.
bool makeFolders(const std::string& folder);

// Gets `file` ready to be linked from a document whose files live in `base`
// (its folder, with a trailing separator): copies it into base/assets/
// unless it is already under `base`. `target` is the link destination,
// relative to `base` and encoded.
bool placeFile(const std::string& file, const std::string& base, std::string& target, std::string& error);
// ![name](target) for images, [name](target) for anything else.
std::string fileLink(std::string_view name, std::string_view target, bool image);

// `markdown` with the line breaks it needs to stand as a paragraph of its
// own between `before` and `after`, the text right before and after the
// insertion point (two bytes of each are enough; empty at the ends).
std::string asBlock(std::string_view before, std::string_view after, std::string_view markdown);

// A link destination in a Markdown text: ](target) or <img src="target">.
struct LinkRef
{
    size_t pos;         // where `target` starts in the text
    std::string target; // as written (encoded)
};
// The destinations that point into assets/.
std::vector<LinkRef> assetLinks(std::string_view text);

// A link destination to rewrite: `length` bytes at `pos` become `target`.
struct Rename
{
    size_t pos;
    size_t length;
    std::string target;
};
// Copies the assets `text` links to from `oldBase` to `newBase` (both
// folders with trailing separators), for Save As or an unsaved document's
// first save. An asset whose name is taken at the new place by a different
// file arrives under a new name; the links to it are returned as renames,
// last position first. Nothing is deleted, so a failed save loses nothing.
std::vector<Rename> transferAssets(std::string_view text, const std::string& oldBase, const std::string& newBase);

} // namespace embed
