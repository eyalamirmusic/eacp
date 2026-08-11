#include "ComponentHost.h"

namespace eacp::UI
{
ComponentHost::ComponentHost()
{
    // No MSAA: it would feather the scissor clip, and the eacp-text glyph
    // pipeline is single-sample only.
    setSampleCount(1);
    setHandlesMouseEvents(true);

    // The tree only hears keys while this one view is the first responder.
    setGrabsFocusOnMouseDown(true);
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

    // Dropped rather than moved on: a focusGained during a subtree's destruction
    // could land on something already gone.
    if (focusedComponent == &component)
        focusedComponent = nullptr;

    if (root == &component)
    {
        root = nullptr;
        lastComponentCount = 0;
        lastPaintedComponents = 0;
        lastDroppedPaths = 0;
        lastMeshedPaths = 0;
        lastSharedMasks = 0;
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

float ComponentHost::getAtlasFillFraction() const
{
    return paths.has_value() ? paths->getFillFraction() : 0.f;
}

int ComponentHost::getAtlasSize() const
{
    return paths.has_value() ? paths->getWidth() : 0;
}

void ComponentHost::setBackgroundColour(const Color& colour)
{
    background = colour;
    repaint();
}

void ComponentHost::setFont(const Font& fontToUse)
{
    font = fontToUse;

    if (text.has_value())
        text->setFont(font);

    // Components that never named a face recorded glyphs of the old one.
    if (root != nullptr)
        markTreeDirty(*root);

    repaint();
}

GradientRamps& ComponentHost::gradientRamps() const
{
    // Built on the first ask, not with the batches: a tree can be painted before
    // it has ever been sized.
    if (!ramps.has_value())
        ramps.emplace();

    return *ramps;
}

Text::TextRenderer& ComponentHost::renderer() const
{
    if (!text.has_value())
    {
        text.emplace(font.pointSize, font.family);
        text->setFont(font);
    }

    return *text;
}

float ComponentHost::measureText(std::string_view textToMeasure,
                                 const Font& fontToUse) const
{
    return renderer().measure(textToMeasure, fontToUse);
}

float ComponentHost::getLineHeight(const Font& fontToUse) const
{
    return renderer().lineHeight(fontToUse);
}

float ComponentHost::getAscent(const Font& fontToUse) const
{
    return renderer().ascent(fontToUse);
}

void ComponentHost::setFontPointSize(float points)
{
    auto updated = font;
    updated.pointSize = points;

    setFont(updated);
}

void ComponentHost::setFontFamily(const std::string& family)
{
    auto updated = font;
    updated.family = family;

    setFont(updated);
}

void ComponentHost::resized()
{
    GPUView::resized();

    auto bounds = getLocalBounds();

    if (bounds.w <= 0.f || bounds.h <= 0.f)
        return;

    // A resize only moves the logical space, leaving the pipelines untouched.
    if (shapes.has_value())
    {
        shapes->setLogicalSize({bounds.w, bounds.h});
        shapes->setPixelScale(backingScale());
        meshes->setLogicalSize({bounds.w, bounds.h});
        layers->setLogicalSize({bounds.w, bounds.h});
    }
    else
    {
        paths.emplace();
        shapes.emplace(*paths,
                       gradientRamps(),
                       Point {bounds.w, bounds.h},
                       backingScale(),
                       sampleCount());
        meshes.emplace(
            *paths, gradientRamps(), Point {bounds.w, bounds.h}, sampleCount());
        layers.emplace(Point {bounds.w, bounds.h}, sampleCount());
    }

    if (root != nullptr)
        root->setBounds(bounds);

    repaint();
}

void ComponentHost::markTreeDirty(Component& component)
{
    component.selfDirty = true;
    component.descendantDirty = true;

    for (auto* child: component.getChildren())
        markTreeDirty(*child);
}

void ComponentHost::record(Component& component)
{
    auto local = component.getLocalBounds();

    component.paintList.clear();
    component.overList.clear();

    // Cleared before the paint, so a repaint() from inside paint() marks the
    // next frame instead of being cleared out from under it.
    component.selfDirty = false;

    {
        auto g = Graphics {component.paintList, gradientRamps(), renderer(), local};
        component.paint(g);
    }

    {
        auto g = Graphics {component.overList, gradientRamps(), renderer(), local};
        component.paintOverChildren(g);
    }

    ++paintedThisWalk;
}

bool ComponentHost::recordComponent(Component& component)
{
    // A hidden subtree is not painted, but what it owes is carried up so it is
    // reached on the frame it is shown again.
    if (!component.isVisible())
        return component.needsRecording();

    if (component.selfDirty)
        record(component);

    auto pending = false;

    if (component.descendantDirty)
        for (auto* child: component.getChildren())
            pending = recordComponent(*child) || pending;

    // Recomputed rather than cleared, which is what keeps repaint()'s early-out
    // sound - see the invariant on Component::descendantDirty.
    component.descendantDirty = pending;

    return pending;
}

int ComponentHost::paintDirtyComponents()
{
    if (root == nullptr)
        return 0;

    recordTree();

    return paintedThisWalk;
}

void ComponentHost::recordTree()
{
    paintedThisWalk = 0;

    // Published only once the walk is over, so a component painting the figure
    // reads the last completed frame's.
    struct Publish
    {
        ~Publish() { host.lastPaintedComponents = host.paintedThisWalk; }
        ComponentHost& host;
    } publish {*this};

    if (!root->needsRecording())
        return;

    auto before = renderer().generation();

    recordComponent(*root);

    if (renderer().generation() == before)
        return;

    // The glyph atlas was cleared mid-walk, so earlier recordings name texels
    // handed to somebody else. Retried once only; a third tick waits a frame.
    markTreeDirty(*root);
    recordComponent(*root);
}

void ComponentHost::playComponent(Component& component,
                                  DrawPlayer& player,
                                  Point origin,
                                  const Rect& clip)
{
    if (!component.isVisible())
        return;

    auto bounds = component.getBounds();
    auto position = Point {origin.x + bounds.x, origin.y + bounds.y};

    // Where this component may draw, in surface points.
    auto visible = clip.intersection({position.x, position.y, bounds.w, bounds.h});

    // The whole subtree goes with it, a child not being able to escape the clip.
    if (visible.isEmpty())
        return;

    player.play(component.paintList, position, visible);

    for (auto* child: component.getChildren())
        playComponent(*child, player, position, visible);

    player.play(component.overList, position, visible);
}

void ComponentHost::markAllPathsDirty(Component& component)
{
    for (auto* shape: component.getPathShapes())
        shape->invalidate();

    for (auto* child: component.getChildren())
        markAllPathsDirty(*child);
}

void ComponentHost::rasterizeDirtyPaths(Component& component,
                                        GPUWidgets::CoverageBatch& batch,
                                        PathWalk& walk)
{
    for (auto* shape: component.getPathShapes())
    {
        if (shape->isDirty())
        {
            shape->rasterize(*paths, masks, backingScale(), batch);

            // Growing or compacting relocates every slot already handed out, so
            // the whole walk has to start again against the new layout.
            if (paths->takeMovedFlag())
                walk.atlasMoved = true;
        }

        // A census, not a tally of this frame: a dropped shape stays missing
        // over frames that rasterize nothing.
        if (shape->wasDropped())
            ++walk.dropped;

        if (shape->isMeshed())
            ++walk.meshed;

        if (shape->isSharingMask())
            ++walk.shared;
    }

    for (auto* child: component.getChildren())
        rasterizeDirtyPaths(*child, batch, walk);
}

// Children before parents, and a component's own layers in registration order,
// which together are what lets one layer hold another.
void ComponentHost::renderLayers(GPU::Frame& frame)
{
    if (root == nullptr || !shapes.has_value())
        return;

    lastRenderedLayers = 0;
    renderLayers(*root, frame);
}

void ComponentHost::renderLayers(Component& component, GPU::Frame& frame)
{
    for (auto* child: component.getChildren())
        renderLayers(*child, frame);

    for (auto* layer: component.getLayers())
        if (layer->isDirty())
            renderLayer(*layer, frame);
}

void ComponentHost::renderLayer(Layer& layer, GPU::Frame& frame)
{
    auto bounds = layer.getBounds();

    if (!layer.ensureTexture(backingScale()))
        return;

    // The tree's own renderers, reused: a layer texture is made to match the
    // drawable's format and sample count.
    auto descriptor = GPU::RenderPassDescriptor {};
    descriptor.clearColor = Color::black(0.f);

    auto pass = frame.beginPass(layer.getTexture(), descriptor);

    shapes->setLogicalSize({bounds.w, bounds.h});
    meshes->setLogicalSize({bounds.w, bounds.h});
    layers->setLogicalSize({bounds.w, bounds.h});

    shapes->begin(pass);
    meshes->begin(pass);

    text->setViewport({bounds.w, bounds.h}, backingScale());
    text->begin();

    {
        // Not kept: the layer's texture is already the cache.
        auto list = DrawList {};

        {
            auto g =
                Graphics {list, *ramps, renderer(), {0.f, 0.f, bounds.w, bounds.h}};

            // Puts the layer's top-left on the texture's, so content draws in
            // the coordinates it was authored in.
            g.translate(-bounds.x, -bounds.y);

            layer.onPaint(g);
        }

        auto player = DrawPlayer {*shapes,
                                  *meshes,
                                  *layers,
                                  *text,
                                  pass,
                                  {0.f, 0.f, bounds.w, bounds.h},
                                  backingScale()};

        player.play(list, {}, {0.f, 0.f, bounds.w, bounds.h});
        player.flush();
    }

    shapes->end();
    meshes->end();

    auto surface = getLocalBounds();

    shapes->setLogicalSize({surface.w, surface.h});
    meshes->setLogicalSize({surface.w, surface.h});
    layers->setLogicalSize({surface.w, surface.h});

    layer.markRendered();
    ++lastRenderedLayers;
}

// Must run before beginPass: a compute pass and a render pass cannot be open at
// once, and the render pass is what samples the atlas.
void ComponentHost::rasterizePaths(GPU::Frame& frame)
{
    if (root == nullptr || !paths.has_value())
        return;

    // Coverage is device pixels, so a scale change invalidates every mask. Told
    // before the walk, so entries are gone before anything asks for one.
    masks.setScale(backingScale());

    if (backingScale() != lastPathScale)
    {
        lastPathScale = backingScale();
        markAllPathsDirty(*root);
    }

    auto generationBefore = paths->generation();

    auto walk = PathWalk {};

    // The first pass may move the atlas under what it has already placed; the
    // second runs against the layout that came out of it.
    constexpr auto maxPlacementAttempts = 2;

    for (auto attempt = 0; attempt < maxPlacementAttempts; ++attempt)
    {
        // A move on the last attempt is unanswerable - a shape placed early
        // would keep a uv into texels since handed to another shape, and draw
        // its coverage. Refused, the mask is merely absent and counted.
        auto isLastAttempt = attempt == maxPlacementAttempts - 1;
        paths->setRelocationAllowed(!isLastAttempt);

        walk = PathWalk {};

        // Gathered rather than dispatched, so a pass the atlas moved under
        // costs no GPU work: beginning the batch again throws it away.
        pathBatch.begin(paths->getTexture());
        rasterizeDirtyPaths(*root, pathBatch, walk);

        if (!walk.atlasMoved)
            break;

        // The slots placed after the move belong to nobody, and everything is
        // about to be rasterized again, so the shelf and its cache start empty.
        markAllPathsDirty(*root);
        paths->forgetAllocations();
        masks.clear();
    }

    if (!pathBatch.isEmpty())
    {
        auto compute = frame.beginCompute();
        pathBatch.dispatch(compute);
    }

    // Every recorded uv now points at texels belonging to somebody else, so this
    // has to happen before the recording walk rather than after it.
    if (paths->generation() != generationBefore)
        markTreeDirty(*root);

    lastMeshedPaths = walk.meshed;
    lastSharedMasks = walk.shared;

    reportDroppedPaths(walk.dropped);
}

// Reported on the change rather than every frame.
void ComponentHost::reportDroppedPaths(int count)
{
    if (count == lastDroppedPaths)
        return;

    lastDroppedPaths = count;
    onPathsDropped(count);
}

void ComponentHost::render(GPU::Frame& frame)
{
    auto bounds = getLocalBounds();

    if (root == nullptr || !shapes.has_value() || bounds.w <= 0.f || bounds.h <= 0.f)
    {
        frame.beginPass({background});
        return;
    }

    renderer();

    text->setViewport({bounds.w, bounds.h}, backingScale());

    // Counted before the walk, so a component painting the figure reads this
    // frame's rather than the last one's.
    lastComponentCount = root->countComponentsInTree();

    // Glyph source rects and mask uvs are in the pixels of the display they
    // were built for.
    if (backingScale() != lastRecordScale)
    {
        lastRecordScale = backingScale();
        markTreeDirty(*root);
    }

    // Before recording, so a walk that moved the atlas is answered by
    // re-recording the uvs before anything records them.
    rasterizePaths(frame);

    // After the masks, which a layer's content is drawn out of, and before the
    // recording walk: a layer with no texture yet would record as nothing.
    renderLayers(frame);

    recordTree();

    text->setViewport({bounds.w, bounds.h}, backingScale());
    text->begin();

    auto pass = frame.beginPass({background});
    shapes->begin(pass);
    meshes->begin(pass);

    auto player =
        DrawPlayer {*shapes, *meshes, *layers, *text, pass, bounds, backingScale()};

    playComponent(*root, player, {}, bounds);

    // Drains both queues in order while the pass is open; the shape batch would
    // otherwise self-drain after it, landing quads on top of the last glyphs.
    player.flush();

    lastClipChanges = player.getClipChangeCount();
    lastRendererSwitches = player.getRendererSwitchCount();
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

void ComponentHost::setFocusedComponent(Component* component)
{
    if (focusedComponent == component)
        return;

    auto* previous = focusedComponent;

    // Assigned before either callback, so hasKeyboardFocus() answers correctly
    // from inside focusLost and focusGained.
    focusedComponent = component;

    if (previous != nullptr)
        previous->focusLost();

    if (focusedComponent != nullptr)
    {
        // The native view, which is what the window hands keys to.
        focus();
        focusedComponent->focusGained();
    }

    repaint();
}

void ComponentHost::moveFocusToPressed(Component* pressed)
{
    for (auto* current = pressed; current != nullptr;
         current = current->getParentComponent())
    {
        if (current->getWantsKeyboardFocus())
        {
            setFocusedComponent(current);
            return;
        }
    }
}

bool ComponentHost::dispatchKey(const eacp::Graphics::KeyEvent& event, bool isDown)
{
    // The root when nothing is focused, so a tree never clicked hears shortcuts.
    auto* target = focusedComponent != nullptr ? focusedComponent : root;

    for (auto* current = target; current != nullptr;
         current = current->getParentComponent())
    {
        if (isDown ? current->keyDown(event) : current->keyUp(event))
            return true;
    }

    return false;
}

bool ComponentHost::moveFocusByTab(const eacp::Graphics::KeyEvent& event)
{
    if (!tabMovesFocus || event.keyCode != eacp::Graphics::KeyCode::Tab)
        return false;

    auto* from = focusedComponent != nullptr ? focusedComponent : root;

    if (from == nullptr)
        return false;

    auto* next = from->nextComponentWantingFocus(!event.modifiers.shift);

    if (next == nullptr)
        return false;

    setFocusedComponent(next);

    return true;
}

void ComponentHost::keyDown(const eacp::Graphics::KeyEvent& event)
{
    if (root == nullptr)
        return;

    // The tree first, traversal second: consuming Tab keeps it.
    if (dispatchKey(event, true))
        return;

    moveFocusByTab(event);
}

void ComponentHost::keyUp(const eacp::Graphics::KeyEvent& event)
{
    if (root != nullptr)
        dispatchKey(event, false);
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
        moveFocusToPressed(mouseDownTarget);
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

    // The pointer may have left the pressed component during the drag.
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

    // Up the tree from the pointer until something consumes it.
    auto* target = root->getComponentAt(event.pos);

    while (target != nullptr)
    {
        if (target->mouseWheelMove(makeEvent(*target, event)))
            return;

        target = target->getParentComponent();
    }
}
} // namespace eacp::UI
