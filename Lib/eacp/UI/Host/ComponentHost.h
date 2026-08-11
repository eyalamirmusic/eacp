#pragma once

#include "../Component/Component.h"
#include "../Render/DrawPlayer.h"

#include <optional>

namespace eacp::UI
{
// The one native view a whole component tree lives in, owning the renderers the
// painter draws through. A frame is two walks: the first paints stale components
// into their own DrawLists, the second replays those lists in tree order.
class ComponentHost : public GPU::GPUView
{
public:
    ComponentHost();
    ~ComponentHost() override;

    // Not owned, and must outlive the host. Resized to fill the host, so its own
    // bounds are ignored.
    void setRootComponent(Component& newRoot);
    Component* getRootComponent() const { return root; }

    void setBackgroundColour(const Color& colour);

    // The face every component starts painting in.
    void setFont(const Font& font);
    const Font& getFont() const { return font; }

    void setFontPointSize(float points);
    void setFontFamily(const std::string& family);

    // Measurement without a paint in progress. Builds the text renderer if the
    // host has not drawn yet, so a component can lay out before the first frame.
    float measureText(std::string_view text, const Font& font) const;
    float getLineHeight(const Font& font) const;
    float getAscent(const Font& font) const;

    // Batch breaks in the last frame; should stay flat as the tree grows.
    int getLastClipChangeCount() const { return lastClipChanges; }
    int getLastComponentCount() const { return lastComponentCount; }

    // How many components were painted, as against how many were drawn.
    int getLastPaintedComponentCount() const { return lastPaintedComponents; }

    // Draws spent alternating between masked and meshed shapes.
    int getLastRendererSwitchCount() const { return lastRendererSwitches; }

    // Layers rendered into their own texture this frame; zero once settled.
    int getLastRenderedLayerCount() const { return lastRenderedLayers; }

    // Shapes the coverage atlas had no room for, which draw as nothing. Non-zero
    // only at the atlas ceiling; a dropped shape returns on the next rebuild.
    int getLastDroppedPathCount() const { return lastDroppedPaths; }

    // Called on the way up and again on the way back down.
    std::function<void(int droppedCount)> onPathsDropped = [](int) {};

    // Shapes drawn as triangles, being too large for a mask. See
    // PathShape::Backing.
    int getLastMeshedPathCount() const { return lastMeshedPaths; }

    // A census of the whole tree, not a tally of this frame.
    int getLastSharedMaskCount() const { return lastSharedMasks; }

    // Room reserved rather than room used: the shelf never gives space back.
    float getAtlasFillFraction() const;
    int getAtlasSize() const;

    // Returns how many were painted. Public because it touches no pass and no
    // drawable, so a tree can be warmed up before it is ever shown.
    int paintDirtyComponents();

    void resized() override;
    void render(GPU::Frame& frame) override;

    // Null sends keys to the root. Setting tells both components (focusLost then
    // focusGained) and asks the window for the keyboard.
    void setFocusedComponent(Component* component);
    Component* getFocusedComponent() const { return focusedComponent; }

    // On by default.
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

    // Drops every pointer the host holds to `component` (root, hover, press
    // capture) - a subclass's root member is destroyed before the host's base.
    void componentDeleted(Component& component);

    // Returns whether this subtree still has recording waiting, which only a
    // hidden one does.
    bool recordComponent(Component& component);

    void recordTree();
    void record(Component& component);

    void playComponent(Component& component,
                       DrawPlayer& player,
                       Point origin,
                       const Rect& clip);

    // For a scale change, an atlas move under the uvs, or a new default face.
    static void markTreeDirty(Component& component);

    struct PathWalk
    {
        bool atlasMoved = false;
        int dropped = 0;
        int meshed = 0;
        int shared = 0;
    };

    // Each into a pass of its own, before the frame's pass opens - a pass cannot
    // begin inside another one.
    void renderLayers(GPU::Frame& frame);
    void renderLayers(Component& component, GPU::Frame& frame);
    void renderLayer(Layer& layer, GPU::Frame& frame);

    // Also before the render pass opens, and for the same reason.
    void rasterizePaths(GPU::Frame& frame);
    void rasterizeDirtyPaths(Component& component,
                             GPUWidgets::CoverageBatch& batch,
                             PathWalk& walk);
    void markAllPathsDirty(Component& component);
    void reportDroppedPaths(int count);

    // Converts the position into `target`'s own space.
    MouseEvent makeEvent(const Component& target,
                         const eacp::Graphics::MouseEvent& event) const;

    void updateHover(Component* target, const eacp::Graphics::MouseEvent& event);
    void setHoveredComponent(Component* component,
                             const eacp::Graphics::MouseEvent& event);

    // Returns whether the focused component or any ancestor consumed it.
    bool dispatchKey(const eacp::Graphics::KeyEvent& event, bool isDown);

    // Focuses `pressed` or its nearest ancestor wanting the keyboard; a press on
    // something that wants none leaves focus alone rather than clearing it.
    void moveFocusToPressed(Component* pressed);

    bool moveFocusByTab(const eacp::Graphics::KeyEvent& event);

    Component* root = nullptr;

    Color background {0.11f, 0.12f, 0.15f, 1.f};

    Font font {defaultUIFontFamily(), 13.f};

    // Built on the first resize, and outlives the batches that read it.
    std::optional<CoverageAtlas> paths;

    // Keyed by geometry, so a shape drawn in forty-eight places is rasterized
    // once. Must be dropped with the atlas's allocations and never apart from
    // them, or an entry names a slot the shelf has given to somebody else.
    MaskCache masks;

    // Mutable and lazily built, painting being possible before the first resize.
    mutable std::optional<GradientRamps> ramps;

    // Held across frames because what it holds is the buffers.
    GPUWidgets::CoverageBatch pathBatch;

    std::optional<ShapeBatch> shapes;

    // Where the shapes too large for the atlas go, drawn into the same pass as
    // the quads so the two interleave in paint order.
    std::optional<MeshBatch> meshes;

    std::optional<LayerRenderer> layers;

    // Mutable so a const measure before the first frame can build it.
    mutable std::optional<Text::TextRenderer> text;

    Text::TextRenderer& renderer() const;

    GradientRamps& gradientRamps() const;

    // The scale everything in the atlas was rasterized at, so a move between
    // displays can be noticed.
    float lastPathScale = 0.f;

    // And the scale the recordings were made at: glyph source rects and mask
    // uvs are in the pixels of the display they were built for.
    float lastRecordScale = 0.f;

    // Where the button went down, in root space.
    Point dragOrigin;
    Point lastMousePosition;

    // The component a press captured: drags and the release go to it wherever
    // the pointer has since travelled.
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

    // Shapes that drew through a mask somebody else had already rasterized.
    int lastSharedMasks = 0;
    int lastRenderedLayers = 0;
};
} // namespace eacp::UI
