#pragma once

#include <string>
#include <string_view>
#include <vector>

// What a tile shows.
enum class WorkspaceType { Editor, Preview, Outline, Terminal };

inline constexpr WorkspaceType kWorkspaceTypes[] = {
    WorkspaceType::Editor, WorkspaceType::Preview, WorkspaceType::Outline, WorkspaceType::Terminal};

const char* workspaceTypeName(WorkspaceType type); // the key in settings.ccl: "editor", ...
const char* workspaceTypeLabel(WorkspaceType type); // for people: "Editor", ...

// One node of a layout: a tile of some workspace type, or a split whose
// children sit side by side (horizontal) or stacked (vertical).
struct LayoutNode
{
    bool tile = false;
    WorkspaceType type = WorkspaceType::Editor; // tiles
    bool horizontal = true;                     // splits
    std::vector<LayoutNode> children;           // splits
    float size = -1.0f; // percent of the parent; < 0 shares what the sized siblings leave
};

struct Layout
{
    std::string name;
    LayoutNode root;
};

// Layouts live in settings.ccl as a split tree:
//
//   layouts =
//     research =
//       split = horizontal
//       outline = 20%
//       editor = 50%
//       right =
//         split = vertical
//         preview = 70%
//         terminal = 30%
//
// A key is a workspace type (repeat it for two tiles of one type) or any
// other name holding a nested block, which is a sub-split; `size = 30%`
// inside such a block sizes it. Read with ccl::parseEntries, since the
// merged ccl::Config would collapse repeated keys.
//
// Reads the `layouts` sections of `text` into `layouts`. A layout replaces
// an earlier one with the same name. Mistakes become warnings, and a layout
// with no usable tile is skipped.
void parseLayouts(std::string_view text, std::vector<Layout>& layouts, std::vector<std::string>& warnings);

// The CCL entries for a layout's body (what goes under `name =`), each line
// indented by `indent` spaces.
std::string layoutToCcl(const LayoutNode& root, int indent);

// `settings` (the text of settings.ccl) with `layouts / name` replaced by,
// or added as, `root`. Only that block changes, so comments and formatting
// elsewhere stay.
std::string replaceLayoutBlock(std::string_view settings, std::string_view name, const LayoutNode& root);

const Layout* findLayout(const std::vector<Layout>& layouts, std::string_view name);
