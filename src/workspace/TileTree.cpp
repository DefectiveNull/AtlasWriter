#include "TileTree.h"

#include "text/Font.h"
#include "text/TextRenderer.h"
#include "ui/Draw.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kDividerGrab = 4.0f; // pixels either side of a divider that grab it
constexpr float kWindowEdgeShare = 0.3f; // of the window, for a tile docked along its edge

using Node = TileTree::Node;

size_t indexOf(const Node& parent, const Node* child)
{
    for (size_t i = 0; i < parent.children.size(); ++i) {
        if (parent.children[i].get() == child)
            return i;
    }
    return 0;
}

float weightSum(const Node& split)
{
    float sum = 0.0f;
    for (const auto& child : split.children)
        sum += child->weight;
    return sum > 0.0f ? sum : 1.0f;
}

// Takes `node` out of its parent without collapsing the parent.
std::unique_ptr<Node> detach(Node* node)
{
    Node* parent = node->parent;
    const size_t index = indexOf(*parent, node);
    std::unique_ptr<Node> owned = std::move(parent->children[index]);
    parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(index));
    owned->parent = nullptr;
    return owned;
}

void insert(Node* split, size_t index, std::unique_ptr<Node> node)
{
    node->parent = split;
    // A newcomer gets an average share.
    node->weight = split->children.empty() ? 1.0f : weightSum(*split) / static_cast<float>(split->children.size());
    split->children.insert(split->children.begin() + static_cast<std::ptrdiff_t>(std::min(index, split->children.size())),
                           std::move(node));
}

Node* firstTile(Node* node)
{
    while (node && !node->isTile())
        node = node->children.empty() ? nullptr : node->children.front().get();
    return node;
}

Node* lastTile(Node* node)
{
    while (node && !node->isTile())
        node = node->children.empty() ? nullptr : node->children.back().get();
    return node;
}

} // namespace

std::unique_ptr<Node> TileTree::makeTile(std::unique_ptr<Workspace> workspace, float weight)
{
    auto node = std::make_unique<Node>();
    node->workspace = std::move(workspace);
    node->weight = weight;
    return node;
}

void TileTree::setRoot(std::unique_ptr<Node> root)
{
    root_ = std::move(root);
    if (root_)
        root_->parent = nullptr;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void TileTree::layout(const SDL_FRect& area, float headerHeight)
{
    area_ = area;
    headerHeight_ = headerHeight;
    if (root_)
        layoutNode(*root_, area);
}

void TileTree::layoutNode(Node& node, const SDL_FRect& rect)
{
    node.rect = rect;
    if (node.isTile()) {
        node.content = rect;
        const float bar = std::min(headerHeight_, rect.h);
        node.content.y += bar;
        node.content.h -= bar;
        node.workspace->setBounds(node.content);
        return;
    }
    const size_t n = node.children.size();
    const float total = node.horizontal ? rect.w : rect.h;
    const float available = std::max(0.0f, total - dividerSize() * static_cast<float>(n - 1));
    const float sum = weightSum(node);
    float pos = node.horizontal ? rect.x : rect.y;
    const float end = pos + total;
    for (size_t i = 0; i < n; ++i) {
        Node& child = *node.children[i];
        float size = std::round(available * child.weight / sum);
        if (i + 1 == n)
            size = std::max(0.0f, end - pos); // the last one takes the rounding
        const SDL_FRect r = node.horizontal ? SDL_FRect{pos, rect.y, size, rect.h} : SDL_FRect{rect.x, pos, rect.w, size};
        layoutNode(child, r);
        pos += size + dividerSize();
    }
}

void TileTree::collect(Node& node, std::vector<Node*>& out) const
{
    if (node.isTile()) {
        out.push_back(&node);
        return;
    }
    for (auto& child : node.children)
        collect(*child, out);
}

std::vector<Node*> TileTree::tiles() const
{
    std::vector<Node*> out;
    if (root_)
        collect(*root_, out);
    return out;
}

void TileTree::forEachTile(const std::function<void(Node&)>& fn) const
{
    for (Node* tile : tiles())
        fn(*tile);
}

bool TileTree::contains(const Node* node) const
{
    const std::vector<Node*> all = tiles();
    return std::find(all.begin(), all.end(), node) != all.end();
}

Node* TileTree::tileAt(float x, float y) const
{
    for (Node* tile : tiles()) {
        if (ui::contains(tile->rect, x, y))
            return tile;
    }
    return nullptr;
}

bool TileTree::dividerAt(float x, float y, Divider& out) const
{
    std::vector<Node*> splits;
    if (root_ && !root_->isTile())
        splits.push_back(root_.get());
    while (!splits.empty()) {
        Node* split = splits.back();
        splits.pop_back();
        if (!ui::contains(split->rect, x, y))
            continue;
        for (size_t i = 0; i + 1 < split->children.size(); ++i) {
            const SDL_FRect& a = split->children[i]->rect;
            const float line = split->horizontal ? a.x + a.w : a.y + a.h; // the divider's near edge
            const float p = split->horizontal ? x : y;
            if (p >= line - kDividerGrab && p <= line + dividerSize() + kDividerGrab) {
                out = {split, i};
                return true;
            }
        }
        for (auto& child : split->children) {
            if (!child->isTile())
                splits.push_back(child.get());
        }
    }
    return false;
}

void TileTree::dragDivider(const Divider& divider, float x, float y)
{
    Node& a = *divider.split->children[divider.index];
    Node& b = *divider.split->children[divider.index + 1];
    const bool h = divider.split->horizontal;
    const float start = h ? a.rect.x : a.rect.y;
    const float end = h ? b.rect.x + b.rect.w : b.rect.y + b.rect.h;
    const float minimum = std::min(std::max(3.0f * headerHeight_, 40.0f), (end - start) * 0.5f);
    const float pos = std::clamp(h ? x : y, start + minimum, end - minimum);
    const float sizeA = pos - start;
    const float sizeB = end - pos - dividerSize();
    if (sizeA + sizeB <= 0.0f)
        return;
    const float total = a.weight + b.weight;
    a.weight = total * sizeA / (sizeA + sizeB);
    b.weight = total - a.weight;
    layoutNode(*divider.split, divider.split->rect);
}

// ---------------------------------------------------------------------------
// Changing the tree
// ---------------------------------------------------------------------------

Node* TileTree::neighbor(const Node* tile, Direction direction) const
{
    const SDL_FRect& r = tile->rect;
    Node* best = nullptr;
    float bestDistance = 0.0f, bestOffset = 0.0f;
    for (Node* other : tiles()) {
        if (other == tile)
            continue;
        const SDL_FRect& o = other->rect;
        float distance = 0.0f, overlap = 0.0f, offset = 0.0f;
        switch (direction) {
        case Direction::Left:
            distance = r.x - (o.x + o.w);
            break;
        case Direction::Right:
            distance = o.x - (r.x + r.w);
            break;
        case Direction::Up:
            distance = r.y - (o.y + o.h);
            break;
        case Direction::Down:
            distance = o.y - (r.y + r.h);
            break;
        }
        if (direction == Direction::Left || direction == Direction::Right) {
            overlap = std::min(r.y + r.h, o.y + o.h) - std::max(r.y, o.y);
            offset = std::fabs((o.y + o.h * 0.5f) - (r.y + r.h * 0.5f));
        } else {
            overlap = std::min(r.x + r.w, o.x + o.w) - std::max(r.x, o.x);
            offset = std::fabs((o.x + o.w * 0.5f) - (r.x + r.w * 0.5f));
        }
        if (distance < -0.5f || overlap <= 0.0f)
            continue;
        if (!best || distance < bestDistance - 0.5f || (distance <= bestDistance + 0.5f && offset < bestOffset)) {
            best = other;
            bestDistance = distance;
            bestOffset = offset;
        }
    }
    return best;
}

Node* TileTree::split(Node* tile, bool horizontal, std::unique_ptr<Workspace> workspace, float share, bool before)
{
    return insertBeside(tile, makeTile(std::move(workspace)), horizontal, share, before);
}

Node* TileTree::insertBeside(Node* tile, std::unique_ptr<Node> added, bool horizontal, float share, bool before)
{
    share = std::clamp(share, 0.05f, 0.95f);
    Node* result = added.get();
    Node* parent = tile->parent;
    if (parent && parent->horizontal == horizontal) {
        // Same direction: a new sibling, taking its share from this tile.
        const size_t index = indexOf(*parent, tile);
        added->weight = tile->weight * share;
        tile->weight *= 1.0f - share;
        added->parent = parent;
        parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(before ? index : index + 1),
                                std::move(added));
        return result;
    }
    // Otherwise the tile becomes a split of itself and the new tile.
    auto group = std::make_unique<Node>();
    group->horizontal = horizontal;
    group->weight = tile->weight;
    group->parent = parent;
    Node* groupPtr = group.get();
    std::unique_ptr<Node> old;
    if (parent) {
        const size_t index = indexOf(*parent, tile);
        old = std::move(parent->children[index]);
        parent->children[index] = std::move(group);
    } else {
        old = std::move(root_);
        root_ = std::move(group);
    }
    old->weight = 1.0f - share;
    old->parent = groupPtr;
    added->weight = share;
    added->parent = groupPtr;
    if (before) {
        groupPtr->children.push_back(std::move(added));
        groupPtr->children.push_back(std::move(old));
    } else {
        groupPtr->children.push_back(std::move(old));
        groupPtr->children.push_back(std::move(added));
    }
    return result;
}

Node* TileTree::addAtEdge(Direction edge, std::unique_ptr<Workspace> workspace, float share)
{
    return insertAtEdge(makeTile(std::move(workspace)), edge, share);
}

Node* TileTree::insertAtEdge(std::unique_ptr<Node> added, Direction edge, float share)
{
    share = std::clamp(share, 0.05f, 0.95f);
    const bool horizontal = edge == Direction::Left || edge == Direction::Right;
    const bool first = edge == Direction::Left || edge == Direction::Up;
    Node* result = added.get();
    if (!root_) {
        added->parent = nullptr;
        added->weight = 1.0f;
        root_ = std::move(added);
        return result;
    }
    if (!root_->isTile() && root_->horizontal == horizontal) {
        const float sum = weightSum(*root_);
        added->weight = sum * share / (1.0f - share);
        added->parent = root_.get();
        root_->children.insert(first ? root_->children.begin() : root_->children.end(), std::move(added));
        return result;
    }
    auto group = std::make_unique<Node>();
    group->horizontal = horizontal;
    std::unique_ptr<Node> old = std::move(root_);
    old->weight = 1.0f - share;
    old->parent = group.get();
    added->weight = share;
    added->parent = group.get();
    if (first) {
        group->children.push_back(std::move(added));
        group->children.push_back(std::move(old));
    } else {
        group->children.push_back(std::move(old));
        group->children.push_back(std::move(added));
    }
    root_ = std::move(group);
    return result;
}

void TileTree::normalize(Node* split)
{
    if (!split || split->isTile() || split->children.size() != 1)
        return;
    std::unique_ptr<Node> only = std::move(split->children[0]);
    only->weight = split->weight;
    Node* parent = split->parent;
    if (!parent) {
        only->parent = nullptr;
        root_ = std::move(only);
        return;
    }
    const size_t index = indexOf(*parent, split);
    only->parent = parent;
    // A split inside a split of the same direction merges into it.
    if (!only->isTile() && only->horizontal == parent->horizontal) {
        Node* inner = only.get();
        const float scale = only->weight / weightSum(*inner);
        std::vector<std::unique_ptr<Node>> grandchildren = std::move(inner->children);
        parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(index));
        size_t at = index;
        for (auto& g : grandchildren) {
            g->weight *= scale;
            g->parent = parent;
            parent->children.insert(parent->children.begin() + static_cast<std::ptrdiff_t>(at++), std::move(g));
        }
        return;
    }
    parent->children[index] = std::move(only); // destroys `split`
}

Node* TileTree::remove(Node* tile)
{
    Node* parent = tile->parent;
    if (!parent) {
        root_.reset();
        return nullptr;
    }
    const size_t index = indexOf(*parent, tile);
    detach(tile); // destroys the tile and its workspace
    Node* focus = index < parent->children.size() ? firstTile(parent->children[index].get())
                                                  : lastTile(parent->children[index - 1].get());
    normalize(parent);
    return focus;
}

void TileTree::move(Node* tile, Direction direction)
{
    const bool horizontal = direction == Direction::Left || direction == Direction::Right;
    const bool forward = direction == Direction::Right || direction == Direction::Down;
    Node* child = tile;
    for (Node* p = tile->parent; p; child = p, p = p->parent) {
        if (p->horizontal != horizontal)
            continue;
        const size_t index = indexOf(*p, child);
        if (child == tile) {
            const long long target = static_cast<long long>(index) + (forward ? 1 : -1);
            if (target < 0 || target >= static_cast<long long>(p->children.size()))
                continue; // at this split's edge: try the one around it
            Node* sibling = p->children[static_cast<size_t>(target)].get();
            if (sibling->isTile()) {
                std::swap(p->children[index], p->children[static_cast<size_t>(target)]);
                std::swap(p->children[index]->weight, p->children[static_cast<size_t>(target)]->weight);
                return;
            }
            // Into the neighboring split, on the side facing this tile.
            std::unique_ptr<Node> owned = detach(tile);
            insert(sibling, forward ? 0 : sibling->children.size(), std::move(owned));
            normalize(p);
            return;
        }
        // The tile is nested inside `child`: it leaves for this split, next to `child`.
        Node* oldParent = tile->parent;
        std::unique_ptr<Node> owned = detach(tile);
        insert(p, forward ? index + 1 : index, std::move(owned));
        normalize(oldParent);
        return;
    }
    // No split in that direction: wrap the whole window in one.
    if (!root_ || root_.get() == tile)
        return;
    Node* oldParent = tile->parent;
    std::unique_ptr<Node> owned = detach(tile);
    normalize(oldParent);
    auto group = std::make_unique<Node>();
    group->horizontal = horizontal;
    std::unique_ptr<Node> old = std::move(root_);
    old->parent = group.get();
    old->weight = 1.0f;
    owned->parent = group.get();
    owned->weight = 1.0f;
    if (forward) {
        group->children.push_back(std::move(old));
        group->children.push_back(std::move(owned));
    } else {
        group->children.push_back(std::move(owned));
        group->children.push_back(std::move(old));
    }
    root_ = std::move(group);
}

void TileTree::resize(Node* tile, Direction direction, float fraction)
{
    const bool horizontal = direction == Direction::Left || direction == Direction::Right;
    const bool forward = direction == Direction::Right || direction == Direction::Down;
    Node* child = tile;
    Node* p = tile->parent;
    while (p && p->horizontal != horizontal) {
        child = p;
        p = p->parent;
    }
    if (!p)
        return;
    const size_t index = indexOf(*p, child);
    const size_t n = p->children.size();
    // The edge that moves: the one on the side we move toward, if there is
    // one, else the opposite one.
    size_t a = 0; // child before the edge
    if (forward)
        a = index + 1 < n ? index : index - 1;
    else
        a = index > 0 ? index - 1 : index;
    Node& before = *p->children[a];
    Node& after = *p->children[a + 1];
    const float sum = weightSum(*p);
    const float minimum = sum * 0.05f;
    float delta = sum * fraction * (forward ? 1.0f : -1.0f);
    delta = std::clamp(delta, minimum - before.weight, after.weight - minimum);
    before.weight += delta;
    after.weight -= delta;
}

std::unique_ptr<Node> TileTree::detachTile(Node* tile)
{
    Node* parent = tile->parent;
    std::unique_ptr<Node> owned = detach(tile);
    normalize(parent); // moves nodes' owners around, never the nodes, so pointers stay good
    return owned;
}

void TileTree::setType(Node* tile, WorkspaceType type,
                       const std::function<std::unique_ptr<Workspace>(WorkspaceType)>& make)
{
    if (tile->workspace->type() == type)
        return;
    std::unique_ptr<Workspace> next;
    for (auto it = tile->stash.begin(); it != tile->stash.end(); ++it) {
        if ((*it)->type() == type) {
            next = std::move(*it);
            tile->stash.erase(it);
            break;
        }
    }
    if (!next)
        next = make(type);
    tile->stash.push_back(std::move(tile->workspace));
    tile->workspace = std::move(next);
    tile->workspace->setBounds(tile->content);
}

void TileTree::swap(Node* a, Node* b)
{
    std::swap(a->workspace, b->workspace);
    std::swap(a->stash, b->stash);
    a->workspace->setBounds(a->content);
    b->workspace->setBounds(b->content);
}

TileTree::Drop TileTree::dropAt(float x, float y, const Node* source) const
{
    Drop out;
    if (!root_ || root_->isTile() || !ui::contains(area_, x, y))
        return out; // a single tile has nowhere to go
    // Along the window's edge: that whole edge.
    const float band = std::max(12.0f, std::round(headerHeight_ * 0.5f));
    const float toEdge[4] = {x - area_.x, area_.x + area_.w - x, y - area_.y, area_.y + area_.h - y};
    int nearest = 0;
    for (int i = 1; i < 4; ++i) {
        if (toEdge[i] < toEdge[nearest])
            nearest = i;
    }
    if (toEdge[nearest] < band) {
        out.kind = Drop::Kind::WindowEdge;
        out.edge = static_cast<Direction>(nearest); // Left, Right, Up, Down
        return out;
    }
    Node* target = tileAt(x, y);
    if (!target || target == source)
        return out;
    const SDL_FRect& r = target->rect;
    // Distance to each edge as a fraction of the tile; the outer quarter is
    // that edge, the nearer one in a corner.
    const float f[4] = {(x - r.x) / r.w, (r.x + r.w - x) / r.w, (y - r.y) / r.h, (r.y + r.h - y) / r.h};
    nearest = 0;
    for (int i = 1; i < 4; ++i) {
        if (f[i] < f[nearest])
            nearest = i;
    }
    out.target = target;
    if (f[nearest] < 0.25f) {
        out.kind = Drop::Kind::Edge;
        out.edge = static_cast<Direction>(nearest);
    } else {
        out.kind = Drop::Kind::Swap;
    }
    return out;
}

SDL_FRect TileTree::dropRect(const Drop& drop) const
{
    const auto half = [](SDL_FRect r, Direction edge, float share) {
        switch (edge) {
        case Direction::Left:
            r.w = std::round(r.w * share);
            break;
        case Direction::Right:
            r.x += r.w - std::round(r.w * share);
            r.w = std::round(r.w * share);
            break;
        case Direction::Up:
            r.h = std::round(r.h * share);
            break;
        case Direction::Down:
            r.y += r.h - std::round(r.h * share);
            r.h = std::round(r.h * share);
            break;
        }
        return r;
    };
    switch (drop.kind) {
    case Drop::Kind::None:
        break;
    case Drop::Kind::Swap:
        return drop.target->rect;
    case Drop::Kind::Edge:
        return half(drop.target->rect, drop.edge, 0.5f);
    case Drop::Kind::WindowEdge:
        return half(area_, drop.edge, kWindowEdgeShare);
    }
    return {};
}

Node* TileTree::drop(Node* source, const Drop& drop)
{
    switch (drop.kind) {
    case Drop::Kind::None:
        return source;
    case Drop::Kind::Swap:
        if (drop.target == source || !contains(drop.target))
            return source;
        swap(source, drop.target);
        return drop.target;
    case Drop::Kind::Edge: {
        if (drop.target == source || !contains(drop.target) || !source->parent)
            return source;
        std::unique_ptr<Node> owned = detachTile(source);
        const bool horizontal = drop.edge == Direction::Left || drop.edge == Direction::Right;
        const bool before = drop.edge == Direction::Left || drop.edge == Direction::Up;
        return insertBeside(drop.target, std::move(owned), horizontal, 0.5f, before);
    }
    case Drop::Kind::WindowEdge: {
        if (!source->parent)
            return source;
        std::unique_ptr<Node> owned = detachTile(source);
        return insertAtEdge(std::move(owned), drop.edge, kWindowEdgeShare);
    }
    }
    return source;
}

namespace {

LayoutNode toLayoutNode(const Node& node)
{
    LayoutNode out;
    if (node.isTile()) {
        out.tile = true;
        out.type = node.workspace->type();
        return out;
    }
    out.horizontal = node.horizontal;
    const float sum = weightSum(node);
    for (const auto& child : node.children) {
        LayoutNode c = toLayoutNode(*child);
        c.size = child->weight / sum * 100.0f;
        out.children.push_back(std::move(c));
    }
    return out;
}

} // namespace

LayoutNode TileTree::toLayout() const
{
    return root_ ? toLayoutNode(*root_) : LayoutNode{true};
}

// ---------------------------------------------------------------------------
// Header and drawing
// ---------------------------------------------------------------------------

namespace {

// A small downward triangle, centered on (cx, cy).
void drawChevron(SDL_Renderer* renderer, float cx, float cy, float size, SDL_FColor color)
{
    const float h = std::round(size * 0.5f);
    const SDL_Vertex v[3] = {
        {{cx - size * 0.5f, cy - h * 0.5f}, color, {0.0f, 0.0f}},
        {{cx + size * 0.5f, cy - h * 0.5f}, color, {0.0f, 0.0f}},
        {{cx, cy + h * 0.5f}, color, {0.0f, 0.0f}},
    };
    SDL_RenderGeometry(renderer, nullptr, v, 3, nullptr, 0);
}

} // namespace

SDL_FRect TileTree::typeButtonRect(const Node& tile, const TextRenderer& text) const
{
    const float em = text.font().pixelSize();
    // Wide enough for every type, so the tabs don't shift when it changes.
    float label = 0.0f;
    for (WorkspaceType type : kWorkspaceTypes)
        label = std::max(label, text.measure(workspaceTypeLabel(type)));
    const float w = std::round(label + 2.6f * em);
    return {tile.rect.x, tile.rect.y, std::min(w, tile.rect.w), tile.content.y - tile.rect.y};
}

SDL_FRect TileTree::closeButtonRect(const Node& tile) const
{
    if (!root_ || root_->isTile())
        return {};
    const float h = tile.content.y - tile.rect.y;
    return {tile.rect.x + tile.rect.w - h, tile.rect.y, h, h};
}

float TileTree::tabWidth(const Node& tile, size_t index, const TextRenderer& text) const
{
    const float em = text.font().pixelSize();
    std::string title = tile.workspace->tabTitle(index);
    if (tile.workspace->tabModified(index))
        title += " \xE2\x80\xA2";
    return std::round(std::clamp(text.measure(title) + 2.0f * em, 6.0f * em, 18.0f * em));
}

SDL_FRect TileTree::tabRect(const Node& tile, size_t index, const TextRenderer& text) const
{
    const SDL_FRect type = typeButtonRect(tile, text);
    float x = type.x + type.w;
    for (size_t i = 0; i < index; ++i)
        x += tabWidth(tile, i, text);
    return {x, tile.rect.y, tabWidth(tile, index, text), tile.content.y - tile.rect.y};
}

TileTree::HeaderHit TileTree::headerAt(const Node& tile, float x, float y, const TextRenderer& text) const
{
    HeaderHit hit;
    const SDL_FRect bar{tile.rect.x, tile.rect.y, tile.rect.w, tile.content.y - tile.rect.y};
    if (!ui::contains(bar, x, y))
        return hit;
    if (ui::contains(typeButtonRect(tile, text), x, y)) {
        hit.part = HeaderHit::Part::Type;
        return hit;
    }
    const SDL_FRect close = closeButtonRect(tile);
    if (close.w > 0.0f && ui::contains(close, x, y)) {
        hit.part = HeaderHit::Part::Close;
        return hit;
    }
    const size_t count = tile.workspace->tabCount();
    if (count >= 2) {
        for (size_t i = 0; i < count; ++i) {
            if (ui::contains(tabRect(tile, i, text), x, y)) {
                hit.part = HeaderHit::Part::Tab;
                hit.tab = i;
                return hit;
            }
        }
    }
    hit.part = HeaderHit::Part::Handle; // a lone tab's title too
    return hit;
}

void TileTree::drawHeader(SDL_Renderer* renderer, TextRenderer& text, const Node& tile, bool menuOpen, bool several)
{
    const SDL_FRect bar{tile.rect.x, tile.rect.y, tile.rect.w, tile.content.y - tile.rect.y};
    if (bar.h <= 0.0f || bar.w <= 0.0f)
        return;
    const SDL_Rect clip = ui::toPixelRect(bar);
    SDL_SetRenderClipRect(renderer, &clip);
    ui::fillRect(renderer, bar, theme::kMenuBarBg);

    const float em = text.font().pixelSize();
    const float lh = static_cast<float>(text.font().lineHeight());
    const float textY = bar.y + std::round((bar.h - lh) * 0.5f);
    Workspace& ws = *tile.workspace;

    // Type button
    const SDL_FRect type = typeButtonRect(tile, text);
    const float inset = std::round(bar.h * 0.15f);
    const SDL_FRect face{type.x + inset, type.y + inset, type.w - 2.0f * inset, type.h - 2.0f * inset};
    ui::fillRect(renderer, face, menuOpen ? theme::kMenuActive : theme::kMenuHover);
    text.draw(face.x + std::round(em * 0.6f), textY, workspaceTypeLabel(ws.type()), theme::kMenuText);
    text.flush();
    drawChevron(renderer, face.x + face.w - std::round(em * 0.9f), bar.y + bar.h * 0.5f, std::round(em * 0.6f),
                theme::kMenuText);
    ui::fillRect(renderer, {type.x + type.w - 1.0f, bar.y, 1.0f, bar.h}, theme::kBorder);

    // Tabs, or the only tab's title
    const SDL_FRect close = several ? closeButtonRect(tile) : SDL_FRect{};
    const float right = close.w > 0.0f ? close.x : bar.x + bar.w;
    const size_t count = ws.tabCount();
    if (count >= 2) {
        for (size_t i = 0; i < count; ++i) {
            const SDL_FRect r = tabRect(tile, i, text);
            if (r.x >= right)
                break;
            const bool active = i == ws.activeTab();
            if (active)
                ui::fillRect(renderer, r, theme::kEditorBg);
            std::string title = ws.tabTitle(i);
            if (ws.tabModified(i))
                title += " \xE2\x80\xA2"; // bullet
            text.draw(r.x + em, textY, title, active ? theme::kMenuText : theme::kMuted, r.x,
                      std::min(r.x + r.w - std::round(em * 0.5f), right));
            ui::fillRect(renderer, {r.x + r.w - 1.0f, r.y, 1.0f, r.h}, theme::kBorder);
        }
    } else if (count == 1) {
        std::string title = ws.tabTitle(0);
        if (ws.tabModified(0))
            title += " \xE2\x80\xA2";
        const float x = type.x + type.w + em;
        text.draw(x, textY, title, theme::kMuted, x, right - std::round(em * 0.5f));
    }

    if (close.w > 0.0f)
        text.draw(std::round(close.x + (close.w - text.advance(0xD7)) * 0.5f), textY, "\xC3\x97", theme::kMuted);
    text.flush();
    ui::fillRect(renderer, {bar.x, bar.y + bar.h - 1.0f, bar.w, 1.0f}, theme::kBorder);
    SDL_SetRenderClipRect(renderer, nullptr);
}

void TileTree::draw(SDL_Renderer* renderer, TextRenderer& text, const Node* focused, const Node* menuTile)
{
    const std::vector<Node*> all = tiles();
    const bool several = all.size() > 1;
    for (Node* tile : all) {
        tile->workspace->draw(renderer);
        SDL_SetRenderClipRect(renderer, nullptr);
        drawHeader(renderer, text, *tile, tile == menuTile, several);
    }

    // Dividers, in the gaps layoutNode() leaves between children.
    std::vector<const Node*> splits;
    if (root_ && !root_->isTile())
        splits.push_back(root_.get());
    while (!splits.empty()) {
        const Node* split = splits.back();
        splits.pop_back();
        for (size_t i = 0; i + 1 < split->children.size(); ++i) {
            const SDL_FRect& a = split->children[i]->rect;
            const SDL_FRect line = split->horizontal
                                     ? SDL_FRect{a.x + a.w, split->rect.y, dividerSize(), split->rect.h}
                                     : SDL_FRect{split->rect.x, a.y + a.h, split->rect.w, dividerSize()};
            ui::fillRect(renderer, line, theme::kBorder);
        }
        for (const auto& child : split->children) {
            if (!child->isTile())
                splits.push_back(child.get());
        }
    }

    // With more than one tile, a line on top of the focused one.
    if (focused && several)
        ui::fillRect(renderer, {focused->rect.x, focused->rect.y, focused->rect.w, 2.0f}, theme::kMenuActive);
}
