#pragma once

#include "../Component/Component.h"
#include "../Render/DrawPlayer.h"

#include <optional>

namespace eacp::UI
{
// The one native view a whole component tree lives in.
//
// A GPUView, so it sits in the ordinary eacp::Graphics::View hierarchy and
// reuses its window, sizing and events -- and so a component tree can be one
// leaf of a larger native layout, or fill a window, or be embedded in a host
// application through Graphics::EmbeddedView, without knowing which.
//
// It owns the renderers the painter draws through and the root component, and
// it converts the native view's events into component-local ones.
//
// Rendering is the View's ordinary on-demand cycle, and a frame is two walks of
// the tree rather than one. The first records: a component whose drawing is
// stale is painted into a DrawList of its own and every other one is stepped
// over, so paint() runs where repaint() was called and nowhere else. The second
// replays those lists into the batches in tree order. Nothing is submitted while
// nothing is dirty, and an animation costs the paint of the one component that
// is animating however large the tree around it is.
class ComponentHost : public GPU::GPUView
{
public:
    ComponentHost();
    ~ComponentHost() override;

    // The tree to draw. The component is not owned and has to outlive the host.
    // It is resized to fill the host, so its own bounds are ignored.
    void setRootComponent(Component& newRoot);
    Component* getRootComponent() const { return root; }

    void setBackgroundColour(const Color& colour);

    // The face every component starts painting in. A component can set another
    // for its own paint(), and they share one glyph atlas, so what this decides
    // is what the tree looks like rather than what it costs.
    void setFont(const Font& font);
    const Font& getFont() const { return font; }

    void setFontPointSize(float points);
    void setFontFamily(const std::string& family);

    // Measurement without a paint in progress -- see Component::measureText,
    // which is where a component should ask. Builds the renderer if the host has
    // not drawn yet: measuring needs the fonts and not the GPU, so a component
    // can lay itself out before the first frame.
    float measureText(std::string_view text, const Font& font) const;
    float getLineHeight(const Font& font) const;
    float getAscent(const Font& font) const;

    // What the last frame cost. `clipChanges` is the number of batch breaks:
    // between two of them every quad goes out as one instanced draw, so this is
    // the figure that should stay flat as the tree grows.
    int getLastClipChangeCount() const { return lastClipChanges; }
    int getLastComponentCount() const { return lastComponentCount; }

    // How many components were actually painted, as against how many were drawn.
    // The figure this tier's redrawing policy is judged by: it is the count of
    // repaint()s the frame answered, so a settled interface reports zero however
    // many components it has, and an animation reports the one that is moving.
    int getLastPaintedComponentCount() const { return lastPaintedComponents; }

    // Draws spent alternating between masked and meshed shapes. See
    // DrawPlayer::getRendererSwitchCount.
    int getLastRendererSwitchCount() const { return lastRendererSwitches; }

    // Layers in the tree that were rendered into a texture of their own this
    // frame, each one a render pass before the frame's. Zero once nothing is
    // changing: a layer whose content is unchanged is drawn from the texture it
    // already has, which is what makes an animated opacity cheap.
    int getLastRenderedLayerCount() const { return lastRenderedLayers; }

    // Vector shapes in the tree that have no mask, the coverage atlas having had
    // no room for them: each one draws as nothing. Zero unless an interface has
    // reached the atlas ceiling, which is a real limit rather than an error --
    // the atlas grows to 4096 square and then compacts, and a tree whose masks
    // do not fit in that at once loses the ones that arrive last.
    //
    // Worth reading somewhere, because nothing else says it happened. A shape
    // dropped this way comes back the next time the atlas is rebuilt -- a
    // resize, a display change, or any later allocation that compacts it.
    int getLastDroppedPathCount() const { return lastDroppedPaths; }

    // Called when that figure changes, for a client that would rather be told
    // than poll. Once on the way up and once on the way back down.
    std::function<void(int droppedCount)> onPathsDropped = [](int) {};

    // Vector shapes in the tree drawn as triangles rather than out of the atlas,
    // being too large for a mask to be worth storing. Zero for an interface,
    // whose shapes are widget-sized; artwork is what meets the threshold, and
    // this is the figure that says how much of a document stopped competing for
    // the atlas. See PathShape::Backing.
    int getLastMeshedPathCount() const { return lastMeshedPaths; }

    // How full the coverage atlas is, and how large it has grown, as the
    // distance to that ceiling while there is still distance to it. Room
    // reserved rather than room used: the shelf never gives space back.
    float getAtlasFillFraction() const;
    int getAtlasSize() const;

    // Paints every component in the tree that has asked for it, into the list
    // the next frame replays, and returns how many were painted.
    //
    // The frame calls this itself, at the top. It is public because it is the
    // half of a frame that touches no pass and no drawable: a tree can be warmed
    // up before it is ever shown, and what a change actually repainted can be
    // asked without drawing anything.
    int paintDirtyComponents();

    void resized() override;
    void render(GPU::Frame& frame) override;

    // The component keys are offered to first. Null means none has been focused,
    // and the tree's keys go to the root -- which is what makes a shortcut work
    // before anything has been clicked.
    //
    // Setting it tells the two components involved (focusLost, then focusGained)
    // and asks the window for the keyboard, a component tree only hearing a key
    // at all if the one native view it lives in is the first responder.
    void setFocusedComponent(Component* component);
    Component* getFocusedComponent() const { return focusedComponent; }

    // Whether Tab moves focus through the tree. On by default; off for a tree
    // where Tab means something else, an editor that indents being the case that
    // wants it.
    void setTabMovesFocus(bool shouldMoveFocus) { tabMovesFocus = shouldMoveFocus; }

    void keyDown(const eacp::Graphics::KeyEvent& event) override;
    void keyUp(const eacp::Graphics::KeyEvent& event) override;

    void mouseDown(const eacp::Graphics::MouseEvent& event) override;
    void mouseUp(const eacp::Graphics::MouseEvent& event) override;
    void mouseDragged(const eacp::Graphics::MouseEvent& event) override;
    void mouseMoved(const eacp::Graphics::MouseEvent& event) override;
    void mouseExited(const eacp::Graphics::MouseEvent& event) override;
    void mouseWheel(const eacp::Graphics::MouseEvent& event) override;

private:
    friend class Component;

    // A component in this tree is going away. Drops every pointer the host
    // holds to it -- the root, the hover, the press capture -- because a host
    // outliving its tree by even one destructor is the normal case: a root held
    // as a member of a ComponentHost subclass is destroyed before the host's
    // own base, so the host would otherwise write through a dead pointer on its
    // way out.
    void componentDeleted(Component& component);

    // Paints every component in the tree whose drawing is stale, into a list of
    // its own, and steps over every subtree that has nothing to record. Returns
    // whether this subtree still has work waiting -- which only a hidden one
    // does, since a visible one is recorded on the frame it is marked.
    bool recordComponent(Component& component);

    void recordTree();
    void record(Component& component);

    // Draws the recorded lists, in tree order, offsetting each by where its
    // component sits and clipping it to what its ancestors left of it.
    void playComponent(Component& component,
                       DrawPlayer& player,
                       Point origin,
                       const Rect& clip);

    // Everything in the subtree has to be painted again: the scale changed, the
    // atlas moved under the uvs, or the tree's default face did.
    static void markTreeDirty(Component& component);

    // What one walk of the tree found: whether the atlas moved under it, how
    // many shapes are on it with no mask, and how many never asked for one.
    struct PathWalk
    {
        bool atlasMoved = false;
        int dropped = 0;
        int meshed = 0;
    };

    // Rendering every Layer in the tree whose content changed, each into a pass
    // of its own, before the frame's own pass opens -- which is the only place
    // it can happen, a pass not being able to begin inside another one.
    void renderLayers(GPU::Frame& frame);
    void renderLayers(Component& component, GPU::Frame& frame);
    void renderLayer(Layer& layer, GPU::Frame& frame);

    // Rasterizing every PathShape in the tree whose geometry changed, before
    // the render pass opens. See the definition for why it can only happen here.
    void rasterizePaths(GPU::Frame& frame);
    void rasterizeDirtyPaths(Component& component,
                             GPUWidgets::CoverageBatch& batch,
                             PathWalk& walk);
    void markAllPathsDirty(Component& component);
    void reportDroppedPaths(int count);

    // Builds the event a component sees: the position converted into its own
    // space, and the fields the native event already carries.
    MouseEvent makeEvent(const Component& target,
                         const eacp::Graphics::MouseEvent& event) const;

    void updateHover(Component* target, const eacp::Graphics::MouseEvent& event);
    void setHoveredComponent(Component* component,
                             const eacp::Graphics::MouseEvent& event);

    // Offers the event to `target` and then to each of its parents, stopping at
    // the first that consumes it. Returns whether any of them did.
    bool dispatchKey(const eacp::Graphics::KeyEvent& event, bool isDown);

    // The pressed component, or the nearest ancestor of it, that wants the
    // keyboard. A press on something that wants nothing leaves focus alone
    // rather than clearing it, so clicking a panel does not silently disarm the
    // editor next to it.
    void moveFocusToPressed(Component* pressed);

    bool moveFocusByTab(const eacp::Graphics::KeyEvent& event);

    Component* root = nullptr;

    Color background {0.11f, 0.12f, 0.15f, 1.f};

    Font font {defaultUIFontFamily(), 13.f};

    // Built on the first resize, once there is a size to build them against.
    // The atlas comes first and outlives the batch that reads it.
    std::optional<CoverageAtlas> paths;

    // The ramps are not among them: painting resolves a gradient to a row, and a
    // tree can be painted before it is sized. Mutable for the same reason the
    // text renderer beside it is -- see gradientRamps().
    mutable std::optional<GradientRamps> ramps;

    // Every path the frame rasterizes, gathered and dispatched as one. Held
    // across frames rather than made per frame, because what it holds is the
    // buffers -- a canvas whose paths all move re-fills them and allocates
    // nothing.
    GPUWidgets::CoverageBatch pathBatch;

    std::optional<ShapeBatch> shapes;

    // Where the shapes too large for the atlas go. Built alongside the quad
    // batch and drawn into the same pass, so the two interleave in the order the
    // tree painted them.
    std::optional<MeshBatch> meshes;

    // What composites a layer's texture back into the picture. One draw apiece
    // and nothing queued, so it holds no state between frames.
    std::optional<LayerRenderer> layers;

    // Mutable because measuring is a const question with a lazily built answer:
    // a component asking for a width before the first frame has to be able to
    // build the renderer to get one.
    mutable std::optional<Text::TextRenderer> text;

    Text::TextRenderer& renderer() const;

    // The ramp table, built on the first ask. Painting needs one and drawing is
    // what the batches are for, so this cannot wait for a size the way they do.
    GradientRamps& gradientRamps() const;

    // The scale everything in the atlas was rasterized at, so a move between
    // displays can be noticed.
    float lastPathScale = 0.f;

    // And the scale the tree's recordings were made at. A glyph's source rect
    // and a mask's uv are both in the pixels of the display they were built
    // for, so a move between two of them makes every list in the tree wrong.
    float lastRecordScale = 0.f;

    // Where the button went down, in root space, carried through the drag so
    // every event can report it.
    Point dragOrigin;
    Point lastMousePosition;

    // The component a press captured. Drags and the matching release go to it
    // wherever the pointer has since travelled -- a fader keeps tracking once
    // the pointer leaves it, which is the behaviour a drag has to have.
    Component* mouseDownTarget = nullptr;
    Component* hoveredComponent = nullptr;
    Component* focusedComponent = nullptr;

    bool tabMovesFocus = true;

    int lastClipChanges = 0;
    int lastRendererSwitches = 0;
    int lastComponentCount = 0;
    int lastPaintedComponents = 0;
    int paintedThisWalk = 0;
    int lastDroppedPaths = 0;
    int lastMeshedPaths = 0;
    int lastRenderedLayers = 0;
};
} // namespace eacp::UI
