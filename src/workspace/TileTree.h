#pragma once

#include "workspace/Layout.h"
#include "workspace/Workspace.h"

#include <SDL3/SDL.h>

#include <functional>
#include <memory>
#include <vector>

class TextRenderer;

enum class Direction { Left, Right, Up, Down };

// The window's tiles, as an i3-style tree: a node is either a tile (one
// workspace) or a split whose children sit side by side (horizontal) or
// stacked (vertical), each taking a share (weight) of the split.
//
// Splits always have two or more children; removing a tile collapses a
// split that is left with one.
class TileTree
{
public:
    struct Node
    {
        Node* parent = nullptr;
        float weight = 1.0f; // share of the parent, relative to the siblings
        // Splits
        bool horizontal = true;
        std::vector<std::unique_ptr<Node>> children;
        // Tiles
        std::unique_ptr<Workspace> workspace;
        // Workspaces the tile showed before its type was changed, at most one
        // of each type; changing back brings one back as it was.
        std::vector<std::unique_ptr<Workspace>> stash;
        SDL_FRect rect{};    // whole tile, header included
        SDL_FRect content{}; // below the header

        bool isTile() const { return workspace != nullptr; }
    };

    // Between children[index] and children[index + 1] of `split`.
    struct Divider
    {
        Node* split = nullptr;
        size_t index = 0;
    };

    static std::unique_ptr<Node> makeTile(std::unique_ptr<Workspace> workspace, float weight = 1.0f);

    void setRoot(std::unique_ptr<Node> root);
    std::unique_ptr<Node> takeRoot() { return std::move(root_); }
    Node* root() const { return root_.get(); }

    // Positions every tile inside `area`. Each tile has a header of
    // `headerHeight` at the top: its type, its tabs, a close button.
    void layout(const SDL_FRect& area, float headerHeight);
    const SDL_FRect& area() const { return area_; }
    float headerHeight() const { return headerHeight_; }

    void forEachTile(const std::function<void(Node&)>& fn) const;
    std::vector<Node*> tiles() const; // in order, left to right / top to bottom
    bool contains(const Node* node) const;
    size_t tileCount() const { return tiles().size(); }

    Node* tileAt(float x, float y) const;
    bool dividerAt(float x, float y, Divider& out) const;
    bool dividerHorizontal(const Divider& divider) const { return divider.split->horizontal; }
    // Moves a divider so it sits at the pointer, keeping both sides usable.
    void dragDivider(const Divider& divider, float x, float y);

    // The tile next to `tile` in `direction`, or null at the window's edge.
    Node* neighbor(const Node* tile, Direction direction) const;

    // Splits `tile`, putting `workspace` in a new tile after it (right or
    // below) that takes `share` of the space. Returns the new tile.
    Node* split(Node* tile, bool horizontal, std::unique_ptr<Workspace> workspace, float share = 0.5f,
                bool before = false);
    // Puts `workspace` along one whole edge of the window.
    Node* addAtEdge(Direction edge, std::unique_ptr<Workspace> workspace, float share);
    // Removes a tile; its space goes to its siblings. Returns the tile that
    // should take the focus (null if the tree is now empty).
    Node* remove(Node* tile);
    // Moves a tile one step in `direction`, i3 style: swaps with the
    // neighboring sibling, or leaves its split for the one beyond it.
    void move(Node* tile, Direction direction);
    // Moves the tile's edge in `direction` by `fraction` of its split.
    void resize(Node* tile, Direction direction, float fraction);

    // Shows a workspace of `type` in the tile: the one it had before, if it
    // has one stashed, else one from `make`. The current one is stashed.
    void setType(Node* tile, WorkspaceType type, const std::function<std::unique_ptr<Workspace>(WorkspaceType)>& make);

    // Mouse docking. A drop lands on a tile's edge (splitting it there), in
    // its middle (swapping the two tiles) or on a window edge.
    struct Drop
    {
        enum class Kind { None, Edge, Swap, WindowEdge } kind = Kind::None;
        Node* target = nullptr; // Edge, Swap
        Direction edge = Direction::Left;
    };
    Drop dropAt(float x, float y, const Node* source) const;
    SDL_FRect dropRect(const Drop& drop) const; // what the drop would cover, for the preview
    // Performs a drop; returns the tile that holds `source`'s workspace now.
    Node* drop(Node* source, const Drop& drop);
    // Swaps what two tiles show; each place keeps its size.
    void swap(Node* a, Node* b);

    // The tree as a layout (sizes as percentages), for "Save layout as".
    LayoutNode toLayout() const;

    // The header: the type button, the tabs (or the only tab's title), the
    // close button (with two or more tiles), and the rest, a drag handle.
    struct HeaderHit
    {
        enum class Part { None, Type, Tab, Close, Handle } part = Part::None;
        size_t tab = 0;
    };
    HeaderHit headerAt(const Node& tile, float x, float y, const TextRenderer& text) const;
    SDL_FRect typeButtonRect(const Node& tile, const TextRenderer& text) const;
    SDL_FRect closeButtonRect(const Node& tile) const; // empty with one tile
    SDL_FRect tabRect(const Node& tile, size_t index, const TextRenderer& text) const;

    // `menuTile`: whose type menu is open, to show its button pressed.
    void draw(SDL_Renderer* renderer, TextRenderer& text, const Node* focused, const Node* menuTile);

private:
    void layoutNode(Node& node, const SDL_FRect& rect);
    void collect(Node& node, std::vector<Node*>& out) const;
    void normalize(Node* split); // collapses one-child splits upward
    // Takes a tile out of the tree (it must not be the root), collapsing
    // its split if that is left with one child.
    std::unique_ptr<Node> detachTile(Node* tile);
    // Puts `node` beside `target`, after it (right or below) unless `before`,
    // taking `share` of the target's space.
    Node* insertBeside(Node* target, std::unique_ptr<Node> node, bool horizontal, float share, bool before);
    Node* insertAtEdge(std::unique_ptr<Node> node, Direction edge, float share);
    float dividerSize() const { return 1.0f; }
    float tabWidth(const Node& tile, size_t index, const TextRenderer& text) const;
    void drawHeader(SDL_Renderer* renderer, TextRenderer& text, const Node& tile, bool menuOpen, bool several);

    std::unique_ptr<Node> root_;
    SDL_FRect area_{};
    float headerHeight_ = 0.0f;
};
