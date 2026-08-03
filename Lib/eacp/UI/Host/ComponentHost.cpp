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

    // A component tree is one native view, so that view has to become the
    // window's first responder for anything inside it to hear a key. Which is
    // what a press on any part of the tree should do anyway -- clicking an
    // interface is how a user says the keyboard belongs to it.
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

    // Dropped rather than moved on. Telling the next component in the focus
    // order that it has the keyboard while a subtree is halfway through being
    // destroyed is how a focusGained lands on something already gone.
    if (focusedComponent == &component)
        focusedComponent = nullptr;

    if (root == &component)
    {
        root = nullptr;
        lastComponentCount = 0;
        lastPaintedComponents = 0;
        lastDroppedPaths = 0;
        lastMeshedPaths = 0;
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

    // Told rather than rebuilt. A family used to be baked into the renderer's
    // atlas, so changing one meant throwing the renderer away; faces coexist in
    // that atlas now, and a change is the default one moving.
    if (text.has_value())
        text->setFont(font);

    // Every component that never named a face of its own recorded glyphs of this
    // one, so all of them have to be laid out again.
    if (root != nullptr)
        markTreeDirty(*root);

    repaint();
}

GradientRamps& ComponentHost::gradientRamps() const
{
    // Built on the first ask rather than with the batches, because painting
    // needs it and drawing is what the batches are for: a tree can be painted
    // before it has ever been sized, and a gradient resolved then has to have
    // somewhere to bake its row.
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

    // A resize only moves the logical space the shaders map from, so the
    // renderer is told rather than rebuilt -- its pipelines are unaffected.
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

    // Cleared before the paint rather than after it, so a component that asks
    // for a repaint from inside its own paint() is marked for the *next* frame
    // rather than having the request cleared out from under it. Bringing that
    // frame about is a separate matter -- see Component::repaint.
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
    // A hidden subtree is not painted, and what it owes is carried rather than
    // forgotten: the walk above it needs to know there is still something down
    // here, or nothing would reach it on the frame it is shown again.
    if (!component.isVisible())
        return component.needsRecording();

    if (component.selfDirty)
        record(component);

    auto pending = false;

    if (component.descendantDirty)
        for (auto* child: component.getChildren())
            pending = recordComponent(*child) || pending;

    // Recomputed rather than cleared, which is what keeps repaint()'s early-out
    // sound: a component below here that is still owed a recording leaves the
    // chain of ancestors marked all the way to the root.
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
    // reads the last completed frame's rather than however much of this one had
    // happened before it was reached.
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

    // A glyph rasterized during the walk filled the atlas and cleared it, so
    // every glyph recorded before that names texels that have been handed to
    // somebody else. Once more, against the atlas that came out of it.
    //
    // Once, and not until it settles. If it ticks again the frame draws as it
    // stands and the next one puts it right, which is the same judgement
    // rasterizePaths makes about its two attempts: a frame with one stale glyph
    // in it is better than a frame that never ends.
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

    // Where this component may draw, in surface points: its own bounds, narrowed
    // by everything its ancestors left of them.
    auto visible = clip.intersection({position.x, position.y, bounds.w, bounds.h});

    // A component scrolled out of its parent, or entirely behind an opaque
    // ancestor's clip, costs nothing at all. The whole subtree goes with it,
    // since a child cannot escape this clip.
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
            shape->rasterize(*paths, backingScale(), batch);

            // Growing or compacting the atlas relocates every slot already
            // handed out, so everything rasterized before this one now points
            // at the wrong texels. Reported up rather than fixed here, because
            // the fix is to start the whole walk again against the new layout.
            if (paths->takeMovedFlag())
                walk.atlasMoved = true;
        }

        // Counted whether or not it was rasterized this frame. A shape the
        // atlas had no room for is missing from every frame until there is room
        // for it, and the frame after the one that dropped it rasterizes
        // nothing at all -- so counting refusals would report the ceiling once
        // and then say the interface was fine while half of it was blank.
        if (shape->wasDropped())
            ++walk.dropped;

        if (shape->isMeshed())
            ++walk.meshed;
    }

    for (auto* child: component.getChildren())
        rasterizeDirtyPaths(*child, batch, walk);
}

// Every layer whose content changed, each rendered into its own texture on this
// frame's command buffer, before the pass that draws the tree.
//
// It has to be here for the same reason the compute dispatch does: a pass cannot
// begin while another is open, so a texture the tree samples has to be finished
// before the tree is drawn. Passes on one frame are ordered by the queue, so a
// layer written here is legal to sample later in the same frame with nothing
// waiting in between.
//
// Children before parents, and a component's own layers in the order they
// registered -- which together are what lets a layer hold another one. See
// Layer's ordering rule, which is the only thing a caller has to obey.
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

    // The renderers are the tree's own, pointed at this pass and told the
    // layer's size. One set rather than a second, because a pipeline is
    // unaffected by what it draws into: the target's format and sample count are
    // the drawable's, which is what a layer texture is made to match.
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
        // Recorded and played straight away rather than kept. A layer already
        // has a cache -- its own texture, which is only rebuilt when the layer
        // is dirty -- so a second one for the drawing that fills it would be a
        // list that is written and read once.
        auto list = DrawList {};

        {
            auto g =
                Graphics {list, *ramps, renderer(), {0.f, 0.f, bounds.w, bounds.h}};

            // So the content draws in the coordinates it was authored in and the
            // layer's own top-left lands on the texture's.
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

// Every path whose geometry changed since the last frame, drawn into the shared
// atlas on this frame's own command buffer.
//
// It has to be here, before beginPass: a compute pass and a render pass cannot
// be open at once, and the render pass is what samples the atlas. Which is also
// why a path is set from resized() or a value change and never from paint() --
// by the time this runs, everything dirty for this frame is already known, so
// the mask a component draws is this frame's rather than the last one's.
void ComponentHost::rasterizePaths(GPU::Frame& frame)
{
    if (root == nullptr || !paths.has_value())
        return;

    if (backingScale() != lastPathScale)
    {
        // Coverage is rasterized in device pixels, so a move to a display of a
        // different scale makes every mask the wrong size for what samples it.
        lastPathScale = backingScale();
        markAllPathsDirty(*root);
    }

    auto generationBefore = paths->generation();

    auto walk = PathWalk {};

    // Twice at most. The first pass may move the atlas under what it has already
    // placed, and the second runs against the layout that came out of it. A
    // third would only be needed if the whole tree's masks could not fit in one
    // atlas at all, and then the honest outcome is that the ones that did not
    // fit are missing rather than that the frame never ends.
    for (auto attempt = 0; attempt < 2; ++attempt)
    {
        // Which is only the honest outcome if the second pass is forbidden to
        // move anything. A move there is unanswerable -- there is no third pass
        // to rebuild what it displaced -- so a shape placed early would keep a
        // uv into texels a later shape has since been handed, and draw that
        // shape's coverage instead of its own. Wrong rather than missing, and
        // silent either way. Refused, the mask is simply absent and counted.
        paths->setRelocationAllowed(attempt == 0);

        walk = PathWalk {};

        // Gathered rather than dispatched, so a first pass the atlas moved under
        // costs no GPU work at all: beginning the batch again is what throws it
        // away, where recording a dispatch per shape meant every one of them had
        // already been issued against a layout that no longer held.
        pathBatch.begin(paths->getTexture());
        rasterizeDirtyPaths(*root, pathBatch, walk);

        if (!walk.atlasMoved)
            break;

        // Everything is about to be rasterized again, so no shape holds a slot
        // and the shelf can start empty. It has to: the first pass kept placing
        // after the move, and those slots belong to nobody now. Left in place
        // they are an atlas-sized leak in the one situation -- a tree that only
        // just fits -- where it decides whether the tree fits at all.
        markAllPathsDirty(*root);
        paths->forgetAllocations();
    }

    if (!pathBatch.isEmpty())
    {
        auto compute = frame.beginCompute();
        pathBatch.dispatch(compute);
    }

    // The atlas grew or compacted, so every uv a component recorded points at
    // texels that belong to somebody else now -- including the components that
    // have nothing else stale about them. They have to be painted again, which
    // is why this happens before the recording walk rather than after it.
    if (paths->generation() != generationBefore)
        markTreeDirty(*root);

    lastMeshedPaths = walk.meshed;
    reportDroppedPaths(walk.dropped);
}

// The atlas ran out and some masks are not on screen. Reported on the change
// rather than every frame, so a client that logs it says so once when an
// interface reaches the ceiling and once when it comes back under it.
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

    // Counted before the walk rather than after it, so a component painting the
    // figure reads this frame's rather than the last one's.
    lastComponentCount = root->countComponentsInTree();

    // A glyph's source rect and a mask's uv are both in the pixels of the
    // display they were built for, so a move to one of a different scale makes
    // every recording in the tree wrong at once.
    if (backingScale() != lastRecordScale)
    {
        lastRecordScale = backingScale();
        markTreeDirty(*root);
    }

    // Before the recording walk, and that is the order rather than an accident:
    // a path is set from resized() or from a value changing, never from paint(),
    // so everything dirty is already known -- and a walk that moved the atlas
    // has to be answered by re-recording the uvs before anything records them.
    rasterizePaths(frame);

    // After the masks: a layer's content is drawn out of the same atlas
    // everything else is, so the coverage has to exist before the layer's own
    // pass runs.
    //
    // And before the tree is recorded, which is the part that is easy to get
    // wrong. A component draws a layer as a quad of a texture, and a layer with
    // no texture yet draws as nothing -- so a tree recorded first would record
    // *nothing* for a layer about to be rendered, and then never look again,
    // there being nothing to tell it the layer had arrived.
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

    // Drains both queues while the pass is still open. The shape batch would
    // drain itself when the pass ends, but that is after this point, so the last
    // run of quads would land on top of the last run of glyphs rather than
    // under it.
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

    // Assigned before either callback, so a component asking whether it has
    // focus from inside focusLost or focusGained is told the truth.
    focusedComponent = component;

    if (previous != nullptr)
        previous->focusLost();

    if (focusedComponent != nullptr)
    {
        // The native view, not the component: what the window hands keys to is
        // this one view, and it can only pass them on if it has them.
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
    // The focused component, or the root when there is none -- so a tree that
    // has never been clicked still hears its shortcuts.
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

    // The tree first, traversal second: a component that wants Tab for itself
    // says so by consuming it, which is the same verdict every other key is
    // decided by rather than a second mechanism.
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
