#include "ComponentHost.h"

#include "../Component/GPUComponent.h"
#include "../Widgets/TouchScroller.h"

#include <algorithm>
#include <ranges>

namespace eacp::UI
{
namespace
{
constexpr auto maximumPathPasses = 9;

// How far an unclipped component's recording reaches either way, in points:
// past any surface, and well inside what a float holds exactly.
constexpr auto unclippedReach = 1.0e6f;

Rect everywhere()
{
    return {-unclippedReach,
            -unclippedReach,
            unclippedReach * 2.f,
            unclippedReach * 2.f};
}

FileDropTarget* asInterestedFileDropTarget(Component& component,
                                           const Vector<std::string>& files)
{
    auto* target = dynamic_cast<FileDropTarget*>(&component);
    return target != nullptr && target->isInterestedInFileDrop(files) ? target
                                                                      : nullptr;
}

Component* visibleChildAt(Component& parent, Point& localPoint)
{
    for (auto* child: std::ranges::reverse_view(parent.getChildren()))
    {
        auto bounds = child->getBounds();
        auto childPoint = Point {localPoint.x - bounds.x, localPoint.y - bounds.y};

        if (child->isVisible() && child->hitTest(childPoint))
        {
            localPoint = childPoint;
            return child;
        }
    }

    return nullptr;
}

bool isWithin(const Component* component, const Component& subtree)
{
    for (; component != nullptr; component = component->getParentComponent())
        if (component == &subtree)
            return true;

    return false;
}
} // namespace

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

    setAcceptsFileDrops(true);
}

ComponentHost::~ComponentHost()
{
    if (root != nullptr)
        root->host = nullptr;
}

// The whole subtree and not just the component: a child it does not own is cut
// loose without being removed, so once it is detached nothing would tell the
// host about it again, and it would be animated or pressed after it is gone.
void ComponentHost::componentDeleted(Component& component)
{
    if (isWithin(hoveredComponent, component))
        hoveredComponent = nullptr;

    if (isWithin(fileDragTarget, component))
        fileDragTarget = nullptr;

    if (isWithin(mouseDownTarget, component))
    {
        mouseDownTarget = nullptr;
        touch = {};
    }

    // Dropped rather than moved on. Telling the next component in the focus
    // order that it has the keyboard while a subtree is halfway through being
    // destroyed is how a focusGained lands on something already gone.
    if (isWithin(focusedComponent, component))
        focusedComponent = nullptr;

    forgetAnimationsIn(component);

    if (root == &component)
    {
        root = nullptr;
        lastComponentCount = 0;
        lastPaintedComponents = 0;
        lastDroppedPaths = 0;
        lastMeshedPaths = 0;
        lastSharedMasks = 0;
        lastImageDraws = 0;
    }
}

void ComponentHost::setRootComponent(Component& newRoot)
{
    if (root != nullptr)
    {
        forgetAnimationsIn(*root);
        root->host = nullptr;
    }

    root = &newRoot;
    root->host = this;
    root->setBounds(getRootBounds());

    subtreeAttached(*root);

    repaint();
}

Rect ComponentHost::getRootBounds() const
{
    auto bounds = getLocalBounds();

    if (!respectsSafeArea)
        return bounds;

    auto inset = bounds.inset(getSafeAreaInsets());

    return {inset.x, inset.y, std::max(0.f, inset.w), std::max(0.f, inset.h)};
}

void ComponentHost::setRespectsSafeArea(bool shouldRespect)
{
    if (respectsSafeArea == shouldRespect)
        return;

    respectsSafeArea = shouldRespect;
    safeAreaInsetsChanged();
}

bool ComponentHost::getRespectsSafeArea() const
{
    return respectsSafeArea;
}

void ComponentHost::safeAreaInsetsChanged()
{
    if (root == nullptr)
        return;

    root->setBounds(getRootBounds());
    repaint();
}

eacp::Graphics::MouseEvent
    ComponentHost::inRootSpace(const eacp::Graphics::MouseEvent& event) const
{
    if (root == nullptr)
        return event;

    auto origin = Point {root->getBounds().x, root->getBounds().y};
    auto result = event;

    result.pos = event.pos - origin;
    result.downPos = event.downPos - origin;

    return result;
}

void ComponentHost::startAnimating(Component& component)
{
    if (!animating.addIfNotThere(&component))
        return;

    startAnimationClock();
}

void ComponentHost::stopAnimating(Component& component)
{
    for (auto*& entry: animating)
        if (entry == &component)
            entry = nullptr;

    if (!advancingAnimations)
        animating.removeAllMatches(static_cast<Component*>(nullptr));

    retireAnimationClockWhenIdle();
}

bool ComponentHost::isAnimating(const Component& component) const
{
    return animating.contains(&component);
}

void ComponentHost::forgetAnimationsIn(Component& subtree)
{
    for (auto*& entry: animating)
        if (isWithin(entry, subtree))
            entry = nullptr;

    if (!advancingAnimations)
        animating.removeAllMatches(static_cast<Component*>(nullptr));

    retireAnimationClockWhenIdle();
}

void ComponentHost::subtreeAttached(Component& subtree)
{
    for (auto* layer: subtree.getLayers())
        if (layer->frameComponent != nullptr)
            layer->frameComponent->resumeIfContinuous();

    for (auto* child: subtree.getChildren())
        subtreeAttached(*child);
}

void ComponentHost::advanceAnimations(double seconds)
{
    advancingAnimations = true;

    // By index, since a component may start another as it advances.
    for (auto index = 0; index < animating.size(); ++index)
    {
        auto* component = animating[index];

        if (component != nullptr && !component->advanceAnimation(seconds))
            animating[index] = nullptr;
    }

    advancingAnimations = false;
    animating.removeAllMatches(static_cast<Component*>(nullptr));

    retireAnimationClockWhenIdle();
}

bool ComponentHost::isAnimating() const
{
    return !animating.empty();
}

void ComponentHost::setAnimationClockEnabled(bool shouldRun)
{
    animationClockEnabled = shouldRun;

    if (!shouldRun)
        animationClock.reset();
    else if (!animating.empty())
        startAnimationClock();
}

void ComponentHost::setAnimationMaxFps(int fps)
{
    animationMaxFps = std::max(0, fps);

    if (animationClock != nullptr)
        animationClock->setMaxFps(animationMaxFps);
}

int ComponentHost::getAnimationMaxFps() const
{
    return animationMaxFps;
}

void ComponentHost::startAnimationClock()
{
    if (!animationClockEnabled || animationClock != nullptr)
        return;

    auto tick = [this](Threads::FrameTime frame) { advanceAnimations(frame.delta); };
    animationClock.create(Threads::DisplayLink::FrameCallback {tick});
    animationClock->setMaxFps(animationMaxFps);
}

// Later rather than now: this is usually running inside the link's own tick,
// and a link cannot be destroyed from there.
void ComponentHost::retireAnimationClockWhenIdle()
{
    if (!animating.empty() || animationClock == nullptr || animationClockRetiring)
        return;

    animationClockRetiring = true;

    auto token = std::weak_ptr<bool> {alive};
    auto retire = [this, token]
    {
        if (token.expired())
            return;

        animationClockRetiring = false;

        if (animating.empty())
            animationClock.reset();
    };

    Threads::callAsync(retire);
}

float ComponentHost::getAtlasFillFraction() const
{
    return paths.has_value() ? paths->getFillFraction() : 0.f;
}

int ComponentHost::getAtlasSize() const
{
    return paths.has_value() ? paths->getWidth() : 0;
}

void ComponentHost::setAtlasQuietWalksToShrink(int walks)
{
    atlasQuietWalks = walks;

    if (paths.has_value())
        paths->setQuietWalksToShrink(walks);
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
    auto bounds = getLocalBounds();

    if (bounds.w <= 0.f || bounds.h <= 0.f)
        return;

    // Only built here. What each of them projects from is set per frame, from
    // the frame's own size -- see setSurfaceSize in render().
    if (!shapes.has_value())
    {
        paths.emplace();
        paths->setQuietWalksToShrink(atlasQuietWalks);
        shapes.emplace(*paths,
                       gradientRamps(),
                       Point {bounds.w, bounds.h},
                       backingScale(),
                       sampleCount());
        meshes.emplace(
            *paths, gradientRamps(), Point {bounds.w, bounds.h}, sampleCount());
        images.emplace(*paths, Point {bounds.w, bounds.h}, sampleCount());
        images->setPixelScale(backingScale());
        layers.emplace(Point {bounds.w, bounds.h}, sampleCount());
    }

    if (root != nullptr)
        root->setBounds(getRootBounds());
}

// A resize only moves the logical space the shaders map from, so every batch is
// told rather than rebuilt -- their pipelines are unaffected by it.
void ComponentHost::setSurfaceSize(Point size)
{
    if (!shapes.has_value())
        return;

    shapes->setLogicalSize(size);
    shapes->setPixelScale(backingScale());
    meshes->setLogicalSize(size);
    images->setLogicalSize(size);
    images->setPixelScale(backingScale());
    layers->setLogicalSize(size);
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

    // Recorded against no edge at all when the component may paint past its
    // own: the walk that plays it back is what cuts it, to its parent's clip.
    if (component.isPaintingUnclipped())
        local = {-unclippedReach,
                 -unclippedReach,
                 unclippedReach * 2.f,
                 unclippedReach * 2.f};

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

    auto paintedBefore = paintedThisWalk;

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

    // A captured subtree recorded anywhere but just before its layer renders --
    // by hand through paintDirtyComponents, or again because the glyph atlas
    // was cleared under this walk -- leaves the layer holding a picture of the
    // recordings it replaced, and the layer has already been rendered for this
    // frame. So it is marked, and the frame after this one draws it again.
    //
    // Judged by what was painted rather than by what is still owed, which a
    // hidden child never recorded goes on owing for as long as it is hidden.
    if (component.capturingLayer != nullptr && &component != recordingContent
        && paintedThisWalk != paintedBefore)
    {
        component.capturingLayer->contentChanged();
        repaint();
    }

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

    if (root->needsRecording())
        recordDirtyTree();

    // A texture the walk stopped drawing -- a recording cleared and painted
    // without it, a component that has gone -- is held by nothing now, and
    // goes with the walk that stopped drawing it. Here rather than in the
    // frame, so a tree painted without one being drawn lets go all the same.
    imageCache.releaseUnused();
}

void ComponentHost::recordDirtyTree()
{
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
    if (component.capturingLayer != nullptr)
        return;

    playSubtree(component, player, origin, clip);
}

void ComponentHost::playSubtree(Component& component,
                                DrawPlayer& player,
                                Point origin,
                                const Rect& clip)
{
    if (!component.isVisible())
        return;

    auto bounds = component.getBounds();
    auto position = Point {origin.x + bounds.x, origin.y + bounds.y};

    // Where this component may draw, in surface points: its own bounds, narrowed
    // by everything its ancestors left of them -- or, for one painting
    // unclipped, everything they left whatever its bounds.
    auto own = Rect {position.x, position.y, bounds.w, bounds.h};
    auto visible = component.isPaintingUnclipped() ? clip : clip.intersection(own);

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
    // A hidden subtree is not drawn, so it takes no room in the atlas either.
    // Its shapes keep their dirty flag rather than losing it -- this walk visits
    // the whole tree every frame, so the first one after the subtree is shown
    // again is the one that rasterizes them, before anything records their uv.
    if (!component.isVisible())
        return;

    for (auto* shape: component.getPathShapes())
    {
        if (shape->isDirty())
        {
            shape->rasterize(*paths, masks, backingScale(), batch);

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

        if (shape->isSharingMask())
            ++walk.shared;
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
    paintedThisWalk = 0;
    renderedGPUComponentsThisWalk = 0;
    culledGPUComponentsThisWalk = 0;
    hiddenGPUComponentsThisWalk = 0;

    auto size = frame.logicalSize();
    renderLayers(*root, frame, {}, {0.f, 0.f, size.x, size.y});

    lastRenderedGPUComponents = renderedGPUComponentsThisWalk;
    lastCulledGPUComponents = culledGPUComponentsThisWalk;
    lastHiddenGPUComponents = hiddenGPUComponentsThisWalk;
}

void ComponentHost::renderLayers(Component& component,
                                 GPU::Frame& frame,
                                 Point origin,
                                 const Rect& clip)
{
    // Skipped while hidden, as its paths are: a layer rendered now would draw
    // shapes that have not been rasterized, and be marked rendered with nothing
    // in it. Left dirty, it renders on the frame the subtree is shown -- after
    // rasterizePaths has given its shapes masks in the atlas as it is then.
    if (!component.isVisible())
    {
        markGPUComponentsHidden(component);
        return;
    }

    auto bounds = component.getBounds();
    auto position = Point {origin.x + bounds.x, origin.y + bounds.y};
    auto own = Rect {position.x, position.y, bounds.w, bounds.h};

    auto reach = component.capturingLayer != nullptr ? everywhere() : clip;

    auto visible = component.isPaintingUnclipped() ? reach : reach.intersection(own);

    // Walked whether or not anything of it is visible: a culled GPUComponent
    // still has to hear that it is, and every other layer renders as it did.
    for (auto* child: component.getChildren())
        renderLayers(*child, frame, position, visible);

    auto visibleLocal = visible.isEmpty() ? Rect {}
                                          : Rect {visible.x - position.x,
                                                  visible.y - position.y,
                                                  visible.w,
                                                  visible.h};

    // An onRender layer samples whatever it likes, and what it is most likely
    // to sample is a layer registered before it -- so one rendering again is
    // taken to mean every onRender layer after it has to as well.
    auto earlierLayerRendered = false;

    for (auto* layer: component.getLayers())
    {
        if (layer->rendersFrame())
        {
            if (renderFrameLayer(*layer, frame, visibleLocal, earlierLayerRendered))
                earlierLayerRendered = true;

            continue;
        }

        if (recordContent(*layer))
            layer->contentChanged();

        if (earlierLayerRendered && layer->rendersOwnPass())
            layer->contentChanged();

        if (layer->needsRendering(backingScale()))
        {
            renderLayer(*layer, frame);
            earlierLayerRendered = true;
        }
    }
}

bool ComponentHost::recordContent(Layer& layer)
{
    auto* content = layer.getContent();

    if (layer.rendersOwnPass() || content == nullptr || !content->isVisible()
        || !content->needsRecording())
        return false;

    recordingContent = content;

    auto paintedBefore = paintedThisWalk;
    auto before = renderer().generation();

    recordComponent(*content);

    // The glyph atlas filled and was cleared, as recordDirtyTree has to allow
    // for -- and answered the same way, against the whole tree, since every
    // recording made before this one names texels that are somebody else's.
    if (renderer().generation() != before)
    {
        markTreeDirty(*root);
        recordComponent(*content);
    }

    recordingContent = nullptr;

    return paintedThisWalk != paintedBefore;
}

void ComponentHost::recordLayer(Layer& layer, DrawList& list)
{
    auto bounds = layer.getBounds();
    auto g = Graphics {list, *ramps, renderer(), {0.f, 0.f, bounds.w, bounds.h}};

    // So the content draws in the coordinates it was authored in and the
    // layer's own top-left lands on the texture's.
    g.translate(-bounds.x, -bounds.y);

    layer.onPaint(g);
}

void ComponentHost::renderLayer(Layer& layer, GPU::Frame& frame)
{
    auto bounds = layer.getBounds();

    if (!layer.ensureTexture(backingScale()))
        return;

    if (layer.rendersOwnPass())
    {
        renderOwnPass(layer, frame);
        return;
    }

    // Recorded before the pass opens, as a component's band is: what the
    // drawing asks for on the way - a picture rendered off-screen, which
    // nests a frame and flushes this one - must not find an encoder open.
    // Played straight away rather than kept: a layer already has a cache --
    // its own texture, which is only rebuilt when the layer is dirty -- so a
    // second one for the drawing that fills it would be a list that is
    // written and read once.
    auto list = std::optional<DrawList> {};

    if (layer.getContent() == nullptr)
        recordLayer(layer, list.emplace());

    // The renderers are the tree's own, pointed at this pass and told the
    // layer's size. One set rather than a second, because a pipeline is
    // unaffected by what it draws into: the target's format and sample count are
    // the drawable's, which is what a layer texture is made to match.
    auto descriptor = GPU::RenderPassDescriptor {};
    descriptor.clearColor = Color::black(0.f);

    auto pass = frame.beginPass(layer.getTexture(), descriptor);

    // The texture is kept across a shrink, so it may be larger than the content.
    // Mapped onto the whole of it, the bounds would be stretched across texels
    // the composite never samples.
    pass.setViewport(layer.getContentArea());

    setSurfaceSize({bounds.w, bounds.h});

    shapes->begin(pass);
    meshes->begin(pass);
    images->begin(pass);

    text->setViewport({bounds.w, bounds.h}, backingScale());
    text->begin();

    {
        auto player = DrawPlayer {*shapes,
                                  *meshes,
                                  *images,
                                  *layers,
                                  *text,
                                  pass,
                                  {0.f, 0.f, bounds.w, bounds.h},
                                  backingScale()};

        if (list)
            player.play(*list, {}, {0.f, 0.f, bounds.w, bounds.h});
        else
            playContent(layer, player);

        player.flush();
    }

    shapes->end();
    meshes->end();
    images->end();

    // Back to the frame's own space for whatever draws into the drawable after
    // this layer's pass.
    setSurfaceSize(frame.logicalSize());

    layer.markRendered();
    ++lastRenderedLayers;
}

// The captured subtree's own recordings, played as the frame would have played
// them, from where the content sits in the owner less the layer's corner. A
// content that is hidden, or no longer below the owner, draws nothing.
void ComponentHost::playContent(Layer& layer, DrawPlayer& player)
{
    auto& content = *layer.getContent();
    auto bounds = layer.getBounds();
    auto origin = Point {-bounds.x, -bounds.y};

    auto* ancestor = content.getParentComponent();

    for (; ancestor != nullptr && ancestor != &layer.owner;
         ancestor = ancestor->getParentComponent())
    {
        if (!ancestor->isVisible())
            return;

        origin.x += ancestor->getBounds().x;
        origin.y += ancestor->getBounds().y;
    }

    if (ancestor == nullptr)
        return;

    playSubtree(content, player, origin, {0.f, 0.f, bounds.w, bounds.h});
}

// A pass of the caller's own, onto the layer's texture and nothing else. The
// tier's renderers are not involved, so neither is the surface size they
// project from.
void ComponentHost::renderOwnPass(Layer& layer, GPU::Frame& frame)
{
    auto descriptor = GPU::RenderPassDescriptor {};
    descriptor.clearColor = Color::black(0.f);
    descriptor.depthAction = GPU::DepthAction::Clear;

    auto area = layer.getContentArea();

    {
        auto pass = frame.beginPass(layer.getTexture(), descriptor);
        pass.setViewport(area);

        layer.onRender(pass, area);
    }

    layer.markRendered();
    ++lastRenderedLayers;
}

// The component's whole render(), with this frame pointed at the layer's
// texture: what it records lands on this frame's command buffer, ahead of the
// pass that composites it.
//
// The bounds are synced rather than set: setting them asks for a frame, and
// this is the frame.
bool ComponentHost::renderFrameLayer(Layer& layer,
                                     GPU::Frame& frame,
                                     const Rect& visibleLocal,
                                     bool earlierLayerRendered)
{
    auto& component = *layer.frameComponent;

    layer.syncFrameBounds(component.getLocalBounds());

    if (component.selfDirty || earlierLayerRendered)
        layer.contentChanged();

    component.resumeIfContinuous();
    component.setVisibility(visibleLocal);
    component.collectGpuTime();

    if (component.wasCulledLastFrame())
    {
        ++culledGPUComponentsThisWalk;
        return false;
    }

    if (!layer.needsRendering(backingScale())
        || !layer.ensureTexture(backingScale()))
        return false;

    {
        auto& target = layer.getTexture();
        auto redirect = GPU::Frame::Redirect {
            frame, target, backingScale(), component.timingLabel()};
        component.renderFrame(frame, target);
    }

    layer.markRendered();
    ++lastRenderedLayers;
    ++renderedGPUComponentsThisWalk;

    return true;
}

void ComponentHost::markGPUComponentsHidden(Component& component)
{
    for (auto* layer: component.getLayers())
    {
        if (layer->frameComponent != nullptr)
        {
            layer->frameComponent->markHidden();
            layer->frameComponent->collectGpuTime();
            ++hiddenGPUComponentsThisWalk;
        }
    }

    for (auto* child: component.getChildren())
        markGPUComponentsHidden(*child);
}

void ComponentHost::dropLayerTextures(Component& component)
{
    for (auto* layer: component.getLayers())
    {
        layer->texture.reset();
        layer->ready = false;
        layer->dirty = true;
    }

    for (auto* child: component.getChildren())
        dropLayerTextures(*child);
}

void ComponentHost::notifyDeviceRestored(Component& component)
{
    for (auto* layer: component.getLayers())
        if (layer->frameComponent != nullptr)
        {
            layer->frameComponent->renderedTarget.reset();
            layer->frameComponent->onDeviceRestored();
        }

    for (auto* child: component.getChildren())
        notifyDeviceRestored(*child);
}

// Nothing made on the old device draws on the new one, so everything is made
// again: the renderers and the atlas by the build resized() does, the glyph
// and ramp textures on their first ask, the images as the tree
// repaints and asks for them, and every layer as it renders.
void ComponentHost::deviceRestored()
{
    shapes.reset();
    meshes.reset();
    images.reset();
    layers.reset();
    paths.reset();
    masks.clear();
    pathBatch.emplace();
    text.reset();
    ramps.reset();
    imageCache.clear();

    lastPathScale = 0.f;
    lastRecordScale = 0.f;

    ComponentHost::resized();

    if (root == nullptr)
        return;

    dropLayerTextures(*root);
    markTreeDirty(*root);
    markAllPathsDirty(*root);
    notifyDeviceRestored(*root);

    repaint();
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

    // Coverage is device pixels, so everything cached was rasterized at one
    // scale and a move to another display drops the lot. Told before the walk
    // rather than after the check below, so that the entries are gone by the
    // time anything asks for one.
    masks.setScale(backingScale());

    if (backingScale() != lastPathScale)
    {
        // Coverage is rasterized in device pixels, so a move to a display of a
        // different scale makes every mask the wrong size for what samples it.
        lastPathScale = backingScale();
        markAllPathsDirty(*root);
    }

    auto generationBefore = paths->generation();

    // Room the tree no longer needs goes back before the walk, which then
    // rasterizes everything into the smaller atlas -- the same answer a grow
    // gets, a frame early rather than mid-walk.
    paths->setRelocationAllowed(true);

    if (paths->shrinkToFit())
    {
        markAllPathsDirty(*root);
        masks.clear();
    }

    auto walk = PathWalk {};

    // Until a pass moves nothing. A pass may move the atlas under what it has
    // already placed, and the next runs against the layout that came out of it.
    // If the whole tree's masks cannot fit in one atlas at all, the honest
    // outcome is that the ones that did not fit are missing rather than that the
    // frame never ends.
    for (auto attempt = 0; attempt < maximumPathPasses; ++attempt)
    {
        // Which is only the honest outcome if the last pass is forbidden to
        // move anything. A move there is unanswerable -- there is no pass after
        // it to rebuild what it displaced -- so a shape placed early would keep
        // a uv into texels a later shape has since been handed, and draw that
        // shape's coverage instead of its own. Wrong rather than missing, and
        // silent either way. Refused, the mask is simply absent and counted.
        paths->setRelocationAllowed(attempt < maximumPathPasses - 1);

        walk = PathWalk {};

        // Gathered rather than dispatched, so a first pass the atlas moved under
        // costs no GPU work at all: beginning the batch again is what throws it
        // away, where recording a dispatch per shape meant every one of them had
        // already been issued against a layout that no longer held.
        pathBatch->begin(paths->getTexture());
        rasterizeDirtyPaths(*root, *pathBatch, walk);

        if (!walk.atlasMoved)
            break;

        // Everything is about to be rasterized again, so no shape holds a slot
        // and the shelf can start empty. It has to: the first pass kept placing
        // after the move, and those slots belong to nobody now. Left in place
        // they are an atlas-sized leak in the one situation -- a tree that only
        // just fits -- where it decides whether the tree fits at all.
        markAllPathsDirty(*root);
        paths->forgetAllocations();

        // With the shelf. Every entry names a slot that is about to be handed
        // out again, so an entry that outlived the shelf would share texels
        // belonging to whatever was placed there second.
        masks.clear();
    }

    if (!pathBatch->isEmpty())
    {
        auto compute = frame.beginCompute();
        pathBatch->dispatch(compute);
    }

    // The atlas grew or compacted, so every uv a component recorded points at
    // texels that belong to somebody else now -- including the components that
    // have nothing else stale about them. They have to be painted again, which
    // is why this happens before the recording walk rather than after it.
    if (paths->generation() != generationBefore)
        markTreeDirty(*root);

    lastMeshedPaths = walk.meshed;
    lastSharedMasks = walk.shared;

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
    // The frame's own size rather than the view's: on a live resize the drawable
    // is already the new one while a size cached at layout is still the old one,
    // and it is the drawable this projection has to agree with.
    const auto size = frame.logicalSize();
    auto bounds = Rect {0.f, 0.f, size.x, size.y};

    if (root == nullptr || !shapes.has_value() || bounds.w <= 0.f || bounds.h <= 0.f)
    {
        frame.beginPass({background});
        return;
    }

    renderer();

    setSurfaceSize(size);

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

    // A captured subtree is recorded as its layer renders, so what was painted
    // there is this frame's painting as much as what the walk paints.
    auto paintedForLayers = paintedThisWalk;

    recordTree();

    lastPaintedComponents += paintedForLayers;

    text->setViewport({bounds.w, bounds.h}, backingScale());
    text->begin();

    auto pass = frame.beginPass({background});
    shapes->begin(pass);
    meshes->begin(pass);
    images->begin(pass);

    auto player = DrawPlayer {
        *shapes, *meshes, *images, *layers, *text, pass, bounds, backingScale()};

    playComponent(*root, player, {}, bounds);

    // Drains every queue while the pass is still open. The batches would drain
    // themselves when the pass ends, but that is after this point, so the last
    // run of quads would land on top of the last run of glyphs rather than
    // under it.
    player.flush();

    lastClipChanges = player.getClipChangeCount();
    lastRendererSwitches = player.getRendererSwitchCount();
    lastImageDraws = images->getDrawCount();
}

MouseEvent ComponentHost::makeEvent(const Component& target,
                                    const eacp::Graphics::MouseEvent& event) const
{
    auto result = MouseEvent {};

    result.position = target.rootPointToLocal(event.pos);
    result.downPosition = target.rootPointToLocal(dragOrigin);
    result.delta = event.delta;
    result.rawDelta = event.rawDelta;
    result.pressure = event.pressure;
    result.scrollPhase = event.scrollPhase;
    result.button = event.button;
    result.modifiers = event.modifiers;
    result.clickCount = event.clickCount;
    result.wheelDelta = event.delta;
    result.preciseWheel = event.preciseScrolling;
    result.magnifyScale = event.magnifyScale;
    result.fromTouch = event.fromTouch;
    result.timestamp = event.timestamp;

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
    }

    updateMouseCursor();
}

void ComponentHost::updateMouseCursor()
{
    if (hoveredComponent != nullptr)
        setMouseCursor(hoveredComponent->getMouseCursor());
    else
        setMouseCursor(eacp::Graphics::MouseCursor::Default);
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

    if (focusedComponent == nullptr)
    {
        setWantsTextInput(false);
    }
    else
    {
        // Set without being applied, so the one focus() below applies it. The
        // native view, not the component: what the window hands keys to is
        // this one view, and it can only pass them on if it has them.
        getProperties().wantsTextInput = focusedComponent->wantsTextInput();
        focus();
        focusedComponent->focusGained();
    }

    repaint();
}

Component* ComponentHost::moveFocusToPressed(Component* pressed)
{
    for (auto* current = pressed; current != nullptr;
         current = current->getParentComponent())
    {
        if (current->getWantsKeyboardFocus())
        {
            setFocusedComponent(current);
            return current;
        }
    }

    return nullptr;
}

void ComponentHost::focusTapped(Component* pressed)
{
    auto* before = focusedComponent;
    auto* focused = moveFocusToPressed(pressed);

    if (focused != nullptr && focused == before && focused->wantsTextInput())
        focus();
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
    // The tree first, traversal second: a component that wants Tab for itself
    // says so by consuming it, which is the same verdict every other key is
    // decided by rather than a second mechanism.
    if (root != nullptr && (dispatchKey(event, true) || moveFocusByTab(event)))
        return;

    passKeyOn();
}

void ComponentHost::keyUp(const eacp::Graphics::KeyEvent& event)
{
    if (root == nullptr || !dispatchKey(event, false))
        passKeyOn();
}

Component*
    ComponentHost::findTouchInterceptor(Component* from,
                                        const eacp::Graphics::MouseEvent& event)
{
    for (auto* current = from; current != nullptr;
         current = current->getParentComponent())
    {
        if (current->interceptsTouch(makeEvent(*current, event)))
            return current;
    }

    return nullptr;
}

void ComponentHost::handTouchTo(Component& interceptor,
                                const eacp::Graphics::MouseEvent& event)
{
    if (mouseDownTarget != nullptr)
        mouseDownTarget->mouseCancel(makeEvent(*mouseDownTarget, event));

    touch.settled = true;
    touch.handedOff = true;
    mouseDownTarget = &interceptor;

    // The gesture is the finger's, not the pressed component's, so nothing
    // under it should go on looking hovered.
    setHoveredComponent(nullptr, event);

    auto atDown = event;
    atDown.pos = dragOrigin;
    atDown.timestamp = touch.downTime;

    interceptor.mouseDown(makeEvent(interceptor, atDown));
}

void ComponentHost::settleTouchDrag(const eacp::Graphics::MouseEvent& event)
{
    if (!touch.active || touch.settled || mouseDownTarget == nullptr)
        return;

    if (mouseDownTarget->claimsTouchDrag(makeEvent(*mouseDownTarget, event)))
    {
        touch.settled = true;
        return;
    }

    auto* above = mouseDownTarget->getParentComponent();

    if (auto* interceptor = findTouchInterceptor(above, event))
        handTouchTo(*interceptor, event);
}

bool ComponentHost::wasTouchTap(const eacp::Graphics::MouseEvent& event) const
{
    return touch.active && !touch.handedOff
           && isWithinTouchSlop(dragOrigin, event.pos);
}

void ComponentHost::mouseDown(const eacp::Graphics::MouseEvent& hostEvent)
{
    if (root == nullptr)
        return;

    auto event = inRootSpace(hostEvent);

    dragOrigin = event.pos;
    lastMousePosition = event.pos;
    pressClickCount = event.clickCount;

    touch = {event.fromTouch, false, event.timestamp};

    mouseDownTarget = root->getComponentAt(event.pos);

    // A finger landing on a list that is still coasting stops it, and is not a
    // tap on whatever the list happens to be carrying past -- nor on the list,
    // so it moves no focus either. The pressed component is asked too, since
    // the finger may have landed on the list itself.
    if (touch.active)
    {
        if (auto* interceptor = findTouchInterceptor(mouseDownTarget, event))
        {
            touch.settled = true;
            touch.handedOff = true;
            mouseDownTarget = interceptor;
        }
    }

    if (mouseDownTarget != nullptr)
    {
        // A finger moves focus when it lifts, and only if it tapped: a press
        // that becomes a scroll must not focus an editor and raise the keyboard.
        if (!touch.active)
            moveFocusToPressed(mouseDownTarget);

        setHoveredComponent(mouseDownTarget, event);
        mouseDownTarget->mouseDown(makeEvent(*mouseDownTarget, event));
    }
}

void ComponentHost::mouseDragged(const eacp::Graphics::MouseEvent& hostEvent)
{
    auto event = inRootSpace(hostEvent);

    lastMousePosition = event.pos;

    settleTouchDrag(event);

    if (mouseDownTarget != nullptr)
        mouseDownTarget->mouseDrag(makeEvent(*mouseDownTarget, event));
}

void ComponentHost::mouseUp(const eacp::Graphics::MouseEvent& hostEvent)
{
    auto event = inRootSpace(hostEvent);

    lastMousePosition = event.pos;

    // Before the release, as a mouse focuses before its press, so a click
    // handler that hands focus somewhere else keeps the last word.
    if (wasTouchTap(event))
        focusTapped(mouseDownTarget);

    if (mouseDownTarget != nullptr)
        mouseDownTarget->mouseUp(makeEvent(*mouseDownTarget, event));

    // After the release, as JUCE sends it, and only to a target the release
    // left alive: componentDeleted clears mouseDownTarget.
    if (mouseDownTarget != nullptr && pressClickCount == 2)
    {
        auto doubleClick = makeEvent(*mouseDownTarget, event);
        doubleClick.clickCount = pressClickCount;
        mouseDownTarget->mouseDoubleClick(doubleClick);
    }

    mouseDownTarget = nullptr;
    pressClickCount = 1;
    touch = {};

    // The pointer may have left the pressed component during the drag, so the
    // hover has to be recomputed from where it actually ended up.
    if (root != nullptr)
        setHoveredComponent(root->getComponentAt(event.pos), event);
}

void ComponentHost::mouseMoved(const eacp::Graphics::MouseEvent& hostEvent)
{
    if (root == nullptr)
        return;

    auto event = inRootSpace(hostEvent);

    lastMousePosition = event.pos;
    updateHover(root->getComponentAt(event.pos), event);
}

void ComponentHost::mouseExited(const eacp::Graphics::MouseEvent& hostEvent)
{
    setHoveredComponent(nullptr, inRootSpace(hostEvent));
}

void ComponentHost::mouseWheel(const eacp::Graphics::MouseEvent& hostEvent)
{
    if (root == nullptr)
        return;

    auto event = inRootSpace(hostEvent);

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

void ComponentHost::mouseMagnify(const eacp::Graphics::MouseEvent& hostEvent)
{
    if (root == nullptr)
        return;

    auto event = inRootSpace(hostEvent);
    auto* target = root->getComponentAt(event.pos);

    while (target != nullptr)
    {
        if (target->mouseMagnify(makeEvent(*target, event)))
            return;

        target = target->getParentComponent();
    }
}

Point ComponentHost::toRootSpace(Point position) const
{
    if (root == nullptr)
        return position;

    return {position.x - root->getBounds().x, position.y - root->getBounds().y};
}

Component* ComponentHost::findFileDropTarget(const Vector<std::string>& files,
                                             Point positionInRoot) const
{
    if (root == nullptr || files.empty() || !root->isVisible()
        || !root->hitTest(positionInRoot))
        return nullptr;

    auto* found = static_cast<Component*>(nullptr);
    auto localPoint = positionInRoot;

    for (auto* component = root; component != nullptr;
         component = visibleChildAt(*component, localPoint))
        if (asInterestedFileDropTarget(*component, files) != nullptr)
            found = component;

    return found;
}

void ComponentHost::setFileDragTarget(Component* target, Point positionInRoot)
{
    if (fileDragTarget == target)
        return;

    if (fileDragTarget != nullptr)
        if (auto* previous = dynamic_cast<FileDropTarget*>(fileDragTarget))
            previous->fileDragExit(fileDragFiles);

    fileDragTarget = target;

    if (fileDragTarget != nullptr)
        if (auto* next = dynamic_cast<FileDropTarget*>(fileDragTarget))
            next->fileDragEnter(fileDragFiles,
                                fileDragTarget->rootPointToLocal(positionInRoot));
}

bool ComponentHost::updateFileDrag(const Vector<std::string>& files, Point position)
{
    auto positionInRoot = toRootSpace(position);
    auto* target = findFileDropTarget(files, positionInRoot);

    fileDragFiles = files;

    if (target != fileDragTarget)
    {
        setFileDragTarget(target, positionInRoot);
        return target != nullptr;
    }

    if (target == nullptr)
        return false;

    if (auto* current = dynamic_cast<FileDropTarget*>(target))
        current->fileDragMove(files, target->rootPointToLocal(positionInRoot));

    return true;
}

bool ComponentHost::fileDragEnter(const Vector<std::string>& files, Point position)
{
    return updateFileDrag(files, position);
}

bool ComponentHost::fileDragMove(const Vector<std::string>& files, Point position)
{
    return updateFileDrag(files, position);
}

void ComponentHost::fileDragExit()
{
    setFileDragTarget(nullptr, {});
    fileDragFiles.clear();
}

void ComponentHost::filesDropped(const Vector<std::string>& files, Point position)
{
    auto positionInRoot = toRootSpace(position);
    auto* target = findFileDropTarget(files, positionInRoot);

    if (target != fileDragTarget)
        setFileDragTarget(nullptr, positionInRoot);

    fileDragTarget = nullptr;
    fileDragFiles.clear();

    if (target != nullptr)
        if (auto* dropTarget = dynamic_cast<FileDropTarget*>(target))
            dropTarget->filesDropped(files,
                                     target->rootPointToLocal(positionInRoot));
}

Component* ComponentHost::getRootComponent() const
{
    return root;
}

const Font& ComponentHost::getFont() const
{
    return font;
}

void ComponentHost::setTheme(const Theme& themeToUse)
{
    theme = themeToUse;

    // The two things the host draws with itself, taken only from a theme that
    // holds them, so a theme of a few colours leaves the face and the backdrop
    // as they were.
    if (auto* face = theme.find(ThemeKeys::font))
        setFont(*face);

    if (auto* backdrop = theme.find(ThemeKeys::background))
        background = *backdrop;

    // Every widget recorded its colours from the theme it found, so all of them
    // have to paint again, as for a change of face.
    if (root != nullptr)
        markTreeDirty(*root);

    repaint();
}

const Theme& ComponentHost::getTheme() const
{
    return theme;
}

ImageCache& ComponentHost::getImageCache()
{
    return imageCache;
}

int ComponentHost::getCachedImageCount() const
{
    return imageCache.size();
}

int ComponentHost::getLastImageDrawCount() const
{
    return lastImageDraws;
}

int ComponentHost::getLastClipChangeCount() const
{
    return lastClipChanges;
}

int ComponentHost::getLastComponentCount() const
{
    return lastComponentCount;
}

int ComponentHost::getLastPaintedComponentCount() const
{
    return lastPaintedComponents;
}

int ComponentHost::getLastRendererSwitchCount() const
{
    return lastRendererSwitches;
}

int ComponentHost::getLastRenderedLayerCount() const
{
    return lastRenderedLayers;
}

int ComponentHost::getLastRenderedGPUComponentCount() const
{
    return lastRenderedGPUComponents;
}

int ComponentHost::getLastCulledGPUComponentCount() const
{
    return lastCulledGPUComponents;
}

int ComponentHost::getLastHiddenGPUComponentCount() const
{
    return lastHiddenGPUComponents;
}

int ComponentHost::getLastDroppedPathCount() const
{
    return lastDroppedPaths;
}

int ComponentHost::getLastMeshedPathCount() const
{
    return lastMeshedPaths;
}

int ComponentHost::getLastSharedMaskCount() const
{
    return lastSharedMasks;
}

Component* ComponentHost::getFocusedComponent() const
{
    return focusedComponent;
}

void ComponentHost::setTabMovesFocus(bool shouldMoveFocus)
{
    tabMovesFocus = shouldMoveFocus;
}
} // namespace eacp::UI
