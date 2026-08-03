#include "Component.h"

#include "../DragAndDrop/DragAndDrop.h"
#include "../Host/ComponentHost.h"

#include <ranges>

namespace eacp::UI
{
Component::Component() = default;

Component::~Component()
{
    // Before unlinking, while walking up to the host still reaches it: the host
    // may be holding this as its root, its hover or its press capture, and any
    // of those outliving the component is a dangling write later.
    if (auto* found = findHost())
        found->componentDeleted(*this);

    // And the same for a drag in progress, which holds the component it started
    // from. A card destroyed mid-drag is the ordinary case rather than a strange
    // one: a drop is what destroys it.
    if (auto* container = findDragContainer())
        container->componentDeleted(*this);

    for (auto* child: children)
        child->parent = nullptr;

    if (parent != nullptr)
        parent->removeChildComponent(*this);
}

void Component::setBounds(const Rect& newBounds)
{
    auto sizeChanged = newBounds.w != bounds.w || newBounds.h != bounds.h;

    bounds = newBounds;

    if (sizeChanged)
    {
        resized();

        // A paint() draws in local bounds, so a component that is now a
        // different size is drawing the wrong picture until it is asked again.
        repaint();
        return;
    }

    // Moved rather than resized: the recording is in this component's own space
    // and the frame applies the position as it replays, so what it has is still
    // right. A dragged card and a scrolled list therefore cost a frame and no
    // paint at all.
    invalidateHost();
}

void Component::setPos(const Rect& ratio)
{
    if (parent != nullptr)
        setBounds(parent->getLocalBounds().getRelative(ratio));
}

void Component::addChildComponent(Component& child)
{
    if (child.parent == this)
        return;

    if (child.parent != nullptr)
        child.parent->removeChildComponent(child);

    child.parent = this;
    children.add(&child);

    // The child is the thing that is new, not this component's own drawing. It
    // was born stale and nothing above it knew, so the chain is joined up here
    // -- otherwise a component added to a tree that is otherwise settled would
    // never be reached by the recording walk.
    if (child.needsRecording())
        child.markAncestorsDirty();

    invalidateHost();
}

void Component::addAndMakeVisible(Component& child)
{
    addChildComponent(child);
    child.setVisible(true);
}

void Component::addChildren(Children childrenToAdd)
{
    for (auto& child: childrenToAdd)
        addAndMakeVisible(child);
}

void Component::removeChildComponent(Component& child)
{
    if (child.parent != this)
        return;

    child.parent = nullptr;
    children.removeAllMatches(&child);

    invalidateHost();
}

void Component::addPathShape(PathShape& shape)
{
    pathShapes.add(&shape);
}

void Component::removePathShape(PathShape& shape)
{
    pathShapes.removeAllMatches(&shape);
}

void Component::addLayer(Layer& layer)
{
    layers.add(&layer);
}

void Component::removeLayer(Layer& layer)
{
    layers.removeAllMatches(&layer);
}

void Component::setVisible(bool shouldBeVisible)
{
    if (visible == shouldBeVisible)
        return;

    visible = shouldBeVisible;

    // Nothing this component draws has changed -- only whether the frame draws
    // it. A component shown again replays the list it was hidden with.
    invalidateHost();

    // Unless it never had one. A subtree hidden before it was ever recorded is
    // skipped by the walk, so its ancestors may have forgotten it; being shown
    // is the point at which that has to be put right.
    if (visible && needsRecording())
        markAncestorsDirty();
}

void Component::toFront()
{
    if (parent == nullptr)
        return;

    parent->children.removeAllMatches(this);
    parent->children.add(this);

    // Order is structure rather than content: the frame walks the child list as
    // it stands, so a reordered sibling is drawn in its new place without either
    // of them painting again.
    invalidateHost();
}

void Component::toBack()
{
    if (parent == nullptr)
        return;

    parent->children.removeAllMatches(this);
    parent->children.insert(0, this);

    invalidateHost();
}

void Component::setInterceptsMouseClicks(bool shouldIntercept)
{
    interceptsMouseClicks = shouldIntercept;
}

void Component::setMouseCursor(eacp::Graphics::MouseCursor cursorToUse)
{
    cursor = cursorToUse;
}

void Component::setWantsKeyboardFocus(bool shouldWantFocus)
{
    wantsKeyboardFocus = shouldWantFocus;

    // Losing it on the way out, because a component that has stopped wanting the
    // keyboard and is still holding it swallows every key the tree sends.
    if (!wantsKeyboardFocus && hasKeyboardFocus())
        giveAwayKeyboardFocus();
}

void Component::grabKeyboardFocus()
{
    // The flag is the single answer to "can this hold the keyboard", so asking
    // does not get around it. Otherwise setWantsKeyboardFocus(false) would mean
    // two different things depending on which side of a grab it was called, and
    // a component could end up focused while reporting that it cannot be.
    if (!wantsKeyboardFocus)
        return;

    if (auto* found = findHost())
        found->setFocusedComponent(this);
}

void Component::giveAwayKeyboardFocus()
{
    if (!hasKeyboardFocus())
        return;

    if (auto* found = findHost())
        found->setFocusedComponent(nullptr);
}

bool Component::hasKeyboardFocus() const
{
    auto* found = findHost();

    return found != nullptr && found->getFocusedComponent() == this;
}

namespace
{
// The tree flattened into the order children were added, which is also the order
// they are painted in. Depth first and parents before children, so a panel that
// wants focus is reached before what it holds.
void gatherFocusOrder(Component& component, Vector<Component*>& into)
{
    if (!component.isVisible())
        return;

    if (component.getWantsKeyboardFocus())
        into.add(&component);

    for (auto* child: component.getChildren())
        gatherFocusOrder(*child, into);
}
} // namespace

Component* Component::nextComponentWantingFocus(bool forwards)
{
    auto* root = this;

    while (root->parent != nullptr)
        root = root->parent;

    auto order = Vector<Component*> {};
    gatherFocusOrder(*root, order);

    if (order.size() == 0)
        return nullptr;

    auto index = -1;

    for (auto i = 0; i < order.size(); ++i)
        if (order[i] == this)
            index = i;

    // Nothing focused yet, or focus on something that has stopped wanting it:
    // start at whichever end the direction implies rather than nowhere.
    if (index < 0)
        return forwards ? order[0] : order[order.size() - 1];

    auto next = forwards ? index + 1 : index - 1;

    // Wrapping, so Tab off the last field returns to the first rather than
    // trapping the keyboard at the end of the tree.
    if (next >= order.size())
        next = 0;
    else if (next < 0)
        next = order.size() - 1;

    return order[next];
}

DragAndDropContainer* Component::findDragContainer() const
{
    for (const auto* current = this; current != nullptr; current = current->parent)
        if (current->dragContainer != nullptr)
            return current->dragContainer;

    return nullptr;
}

ComponentHost* Component::findHost() const
{
    auto* current = this;

    while (current->parent != nullptr)
        current = current->parent;

    return current->host;
}

void Component::repaint()
{
    // The ancestor walk only where the bit was not already set. If it was, they
    // were marked when it was set and nothing has cleared them since -- the
    // recording walk cannot, because it recomputes descendantDirty from what is
    // still pending below rather than clearing it on the way out.
    if (!selfDirty)
    {
        selfDirty = true;
        markAncestorsDirty();
    }

    // And always the host, even for a component already stale: a repaint asked
    // for after this frame's recording walk has run has to bring about the next
    // frame, or the change waits for whatever happens to invalidate the view
    // next.
    invalidateHost();
}

void Component::markAncestorsDirty()
{
    for (auto* current = parent; current != nullptr && !current->descendantDirty;
         current = current->parent)
        current->descendantDirty = true;
}

void Component::invalidateHost()
{
    if (auto* found = findHost())
        found->repaint();
}

float Component::measureText(std::string_view text, const Font& font) const
{
    auto* found = findHost();

    return found != nullptr ? found->measureText(text, font) : 0.f;
}

Font Component::getHostFont() const
{
    auto* found = findHost();

    return found != nullptr ? found->getFont() : Font {};
}

bool Component::hitTest(Point localPoint) const
{
    return getLocalBounds().contains(localPoint);
}

Component* Component::getComponentAt(Point localPoint)
{
    if (!visible || !hitTest(localPoint))
        return nullptr;

    for (auto* child: std::ranges::reverse_view(children))
    {
        auto childBounds = child->getBounds();
        auto childPoint =
            Point {localPoint.x - childBounds.x, localPoint.y - childBounds.y};

        if (auto* hit = child->getComponentAt(childPoint))
            return hit;
    }

    return interceptsMouseClicks ? this : nullptr;
}

Point Component::localPointToRoot(Point localPoint) const
{
    auto result = localPoint;
    auto* current = this;

    while (current->parent != nullptr)
    {
        result.x += current->bounds.x;
        result.y += current->bounds.y;
        current = current->parent;
    }

    return result;
}

Point Component::rootPointToLocal(Point rootPoint) const
{
    auto offset = localPointToRoot({});
    return {rootPoint.x - offset.x, rootPoint.y - offset.y};
}

int Component::countComponentsInTree() const
{
    auto total = 1;

    for (const auto* child: children)
        total += child->countComponentsInTree();

    return total;
}
} // namespace eacp::UI
