#include "ComponentHost.h"

namespace eacp::UI
{
ComponentHost::ComponentHost()
{
    // No multisampling, for two reasons that point the same way. The clip is a
    // scissor rect, and MSAA would feather its edge across a pixel -- exactly
    // what clipping exists to avoid. And the glyph pipeline in eacp-text is
    // built for a single sample, so a multisampled pass could not draw text at
    // all. Neither is a loss here: a component interface is axis-aligned
    // rectangles and glyphs, both of which land crisper without it.
    setSampleCount(1);
    setHandlesMouseEvents(true);
}

ComponentHost::~ComponentHost()
{
    if (root != nullptr)
        root->host = nullptr;
}

void ComponentHost::componentDeleted(Component& component)
{
    if (hoveredComponent == &component)
        hoveredComponent = nullptr;

    if (mouseDownTarget == &component)
        mouseDownTarget = nullptr;

    if (root == &component)
    {
        root = nullptr;
        lastComponentCount = 0;
    }
}

void ComponentHost::setRootComponent(Component& newRoot)
{
    if (root != nullptr)
        root->host = nullptr;

    root = &newRoot;
    root->host = this;
    root->setBounds(getLocalBounds());

    repaint();
}

void ComponentHost::setBackgroundColour(const Color& colour)
{
    background = colour;
    repaint();
}

void ComponentHost::setFontPointSize(float points)
{
    fontPointSize = points;

    if (text.has_value())
        text->setPointSize(points);

    repaint();
}

void ComponentHost::setFontFamily(const std::string& family)
{
    fontFamily = family;

    // The renderer bakes its family at construction, so a change rebuilds it
    // rather than reaching in. Cheap, and it only happens on an explicit call.
    text.reset();
    repaint();
}

void ComponentHost::resized()
{
    GPUView::resized();

    auto bounds = getLocalBounds();

    if (bounds.w <= 0.f || bounds.h <= 0.f)
        return;

    // A resize only moves the logical space the shaders map from, so the
    // renderer is told rather than rebuilt -- its pipelines are unaffected.
    if (sprites.has_value())
        sprites->setLogicalSize({bounds.w, bounds.h});
    else
        sprites.emplace(Point {bounds.w, bounds.h}, sampleCount());

    if (root != nullptr)
        root->setBounds(bounds);

    repaint();
}

void ComponentHost::paintComponent(Component& component, Graphics& g)
{
    if (!component.isVisible())
        return;

    auto scope = Graphics::ScopedState {g};

    auto bounds = component.getBounds();
    g.translate(bounds.x, bounds.y);
    g.reduceClipRegion(component.getLocalBounds());

    // A component scrolled out of its parent, or entirely behind an opaque
    // ancestor's clip, costs nothing at all -- not even the paint() call. The
    // whole subtree goes with it, since a child cannot escape this clip.
    if (g.isClipEmpty())
        return;

    component.paint(g);

    for (auto* child: component.getChildren())
        paintComponent(*child, g);

    component.paintOverChildren(g);
}

void ComponentHost::render(GPU::Frame& frame)
{
    auto bounds = getLocalBounds();

    if (root == nullptr || !sprites.has_value() || bounds.w <= 0.f
        || bounds.h <= 0.f)
    {
        frame.beginPass({background});
        return;
    }

    if (!text.has_value())
        text.emplace(fontPointSize, fontFamily);

    text->setViewport({bounds.w, bounds.h}, backingScale());
    text->begin();

    auto pass = frame.beginPass({background});
    sprites->begin(pass);

    auto g = Graphics {*sprites, *text, pass, bounds, backingScale()};

    paintComponent(*root, g);

    // Drains both queues while the pass is still open. The sprite renderer
    // would drain itself when the pass ends, but that is after this point, so
    // the last run of quads would land on top of the last run of glyphs rather
    // than under it.
    g.flush();

    lastClipChanges = g.getClipChangeCount();
    lastComponentCount = root->countComponentsInTree();
}

MouseEvent ComponentHost::makeEvent(const Component& target,
                                    const eacp::Graphics::MouseEvent& event) const
{
    auto result = MouseEvent {};

    result.position = target.rootPointToLocal(event.pos);
    result.downPosition = target.rootPointToLocal(dragOrigin);
    result.delta = event.delta;
    result.button = event.button;
    result.modifiers = event.modifiers;
    result.clickCount = event.clickCount;
    result.wheelDelta = event.delta;
    result.preciseWheel = event.preciseScrolling;

    return result;
}

void ComponentHost::setHoveredComponent(Component* component,
                                        const eacp::Graphics::MouseEvent& event)
{
    if (hoveredComponent == component)
        return;

    if (hoveredComponent != nullptr)
    {
        hoveredComponent->mouseOver = false;
        hoveredComponent->mouseExit(makeEvent(*hoveredComponent, event));
    }

    hoveredComponent = component;

    if (hoveredComponent != nullptr)
    {
        hoveredComponent->mouseOver = true;
        hoveredComponent->mouseEnter(makeEvent(*hoveredComponent, event));
        setMouseCursor(hoveredComponent->getMouseCursor());
    }
    else
    {
        setMouseCursor(eacp::Graphics::MouseCursor::Default);
    }
}

void ComponentHost::updateHover(Component* target,
                                const eacp::Graphics::MouseEvent& event)
{
    setHoveredComponent(target, event);

    if (target != nullptr)
        target->mouseMove(makeEvent(*target, event));
}

void ComponentHost::mouseDown(const eacp::Graphics::MouseEvent& event)
{
    if (root == nullptr)
        return;

    dragOrigin = event.pos;
    lastMousePosition = event.pos;

    mouseDownTarget = root->getComponentAt(event.pos);

    if (mouseDownTarget != nullptr)
    {
        setHoveredComponent(mouseDownTarget, event);
        mouseDownTarget->mouseDown(makeEvent(*mouseDownTarget, event));
    }
}

void ComponentHost::mouseDragged(const eacp::Graphics::MouseEvent& event)
{
    lastMousePosition = event.pos;

    if (mouseDownTarget != nullptr)
        mouseDownTarget->mouseDrag(makeEvent(*mouseDownTarget, event));
}

void ComponentHost::mouseUp(const eacp::Graphics::MouseEvent& event)
{
    lastMousePosition = event.pos;

    if (mouseDownTarget != nullptr)
        mouseDownTarget->mouseUp(makeEvent(*mouseDownTarget, event));

    mouseDownTarget = nullptr;

    // The pointer may have left the pressed component during the drag, so the
    // hover has to be recomputed from where it actually ended up.
    if (root != nullptr)
        setHoveredComponent(root->getComponentAt(event.pos), event);
}

void ComponentHost::mouseMoved(const eacp::Graphics::MouseEvent& event)
{
    if (root == nullptr)
        return;

    lastMousePosition = event.pos;
    updateHover(root->getComponentAt(event.pos), event);
}

void ComponentHost::mouseExited(const eacp::Graphics::MouseEvent& event)
{
    setHoveredComponent(nullptr, event);
}

void ComponentHost::mouseWheel(const eacp::Graphics::MouseEvent& event)
{
    if (root == nullptr)
        return;

    // To whatever is under the pointer, then up the tree until something
    // consumes it -- a row inside a list does not scroll, but the list holding
    // it does, and the pointer is over the row.
    auto* target = root->getComponentAt(event.pos);

    while (target != nullptr)
    {
        if (target->mouseWheelMove(makeEvent(*target, event)))
            return;

        target = target->getParentComponent();
    }
}
} // namespace eacp::UI
