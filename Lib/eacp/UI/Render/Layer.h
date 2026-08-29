#pragma once

#include "../Common.h"

#include <eacp/GPU/GPU.h>
#include <eacp/GPUWidgets/Path/AffineTransform.h>

#include <functional>
#include <optional>

namespace eacp::UI
{
class Component;
class Graphics;

// A run of drawing rendered into a texture of its own, so it can be composited
// as one thing rather than a shape at a time.
//
// Which is the whole reason it exists, and the difference is only visible where
// the run overlaps itself. Fading twenty shapes by multiplying each one's alpha
// fades twenty shapes; fading the *group* draws them opaque into a texture and
// fades that, and every overlap inside it stays as solid as it was drawn. SVG
// calls the first a presentation attribute on each element and the second
// `opacity` on a group, spells them identically, and means quite different
// pictures.
//
// It is a member of the component that draws it, exactly as PathShape is, and
// for the same reason: the content has to be rendered into the texture *before*
// the frame's own pass opens, because a pass cannot begin inside another one. So
// the host walks the tree, runs every layer's onPaint into its own pass, and the
// tree's own paint() then draws the result as a quad.
//
//   struct Group final : Component
//   {
//       Group() : layer(*this)
//       {
//           layer.onPaint = [this](Graphics& g) { paintContent(g); };
//       }
//
//       void resized() override { layer.setBounds(getLocalBounds()); }
//       void paint(Graphics& g) override { g.drawLayer(layer); }
//
//       Layer layer;
//   };
//
// What it costs is a texture the size of its bounds in device pixels, a render
// pass of its own every time it is rebuilt, and one draw where it is composited.
// A layer whose content does not change is rendered once and drawn as a quad
// ever after -- setDirty is what asks for it again.
//
// **A layer may hold another layer, and the inner one has to exist first.** The
// host renders them in the order they registered, so a layer drawing another
// must be constructed after the one it draws. Building innermost-out is what a
// tree walk gives you anyway, and it is the only ordering rule here.
class Layer
{
public:
    explicit Layer(Component& ownerToUse);
    ~Layer();

    Layer(const Layer&) = delete;
    Layer& operator=(const Layer&) = delete;

    // Where the content lives, in the owning component's points. The texture is
    // this rounded out to whole device pixels, so a layer costs its own area
    // and not the tree's -- keep it to what is actually drawn.
    void setBounds(const Rect& newBounds);
    Rect getBounds() const { return bounds; }

    // Multiplied into the whole layer as it is composited, which is the point of
    // having one.
    void setOpacity(float newOpacity);
    float getOpacity() const { return opacity; }

    // Applied to the quad as it is composited, so a layer can be rotated,
    // scaled or skewed where nothing else in the tier can: the clip is a
    // scissor rect and a rotated one cannot be expressed, but a layer is a
    // picture placed with four corners and they may go anywhere.
    //
    // In the layer's *own* space, with (0, 0) at its top-left rather than the
    // component's -- so a caller turning a box about its middle names half its
    // size and not where it happens to sit, and a layer that holds another
    // composites the same wherever the outer one put it. Move it by setting
    // the bounds; the matrix says what happens inside them.
    //
    // Not a re-render, exactly as the opacity is not: what the texture holds is
    // the content as it was drawn, and this is read where it is composited. So
    // an animated transform costs a frame and no pass of its own, which is what
    // makes it worth doing here rather than in the drawing.
    void setTransform(const GPUWidgets::AffineTransform& newTransform);
    const GPUWidgets::AffineTransform& getTransform() const { return transform; }

    // What goes in it, drawn with the origin at the layer's own top-left -- so a
    // caller draws in the same coordinates it would have drawn in without a
    // layer, and moving the layer moves the content with it.
    //
    // Called by the host, before the frame's pass, and only when the layer is
    // dirty. Non-null by default so nothing has to check.
    std::function<void(Graphics&)> onPaint = [](Graphics&) {};

    // Asks for the content to be rendered again at the top of the next frame.
    // Setting the bounds does this for you; a change of what onPaint would draw
    // does not, since nothing here can see it.
    void setDirty();

    // True once there is a texture with the content in it. Drawing an empty
    // layer is a no-op.
    bool isEmpty() const { return !ready; }

    const GPU::Texture& getTexture() const { return *texture; }

    // The part of that texture the content occupies, in normalised coordinates.
    // Not the whole of it: the texture is kept across a shrink, so a layer that
    // got smaller draws out of the corner it filled rather than reallocating.
    Rect getContentUV() const;

private:
    friend class ComponentHost;

    bool isDirty() const { return dirty; }

    // Makes or grows the texture for the bounds at this scale. False when the
    // bounds hold no pixels, or the device could not give a texture -- either
    // way the layer draws as nothing. Kept between renders and only remade when
    // it is too small, so a layer redrawn every frame allocates nothing.
    bool ensureTexture(float scale);

    // The content has just been rendered into the texture, at the scale
    // ensureTexture was given.
    void markRendered();

    Component& owner;

    Rect bounds;
    float opacity = 1.f;
    GPUWidgets::AffineTransform transform;

    std::optional<GPU::Texture> texture;

    // What of the texture the content actually occupies, in texels: the bounds
    // at the scale they were rendered at. The texture may be larger, being kept
    // across a shrink.
    int contentWidth = 0;
    int contentHeight = 0;

    float renderedScale = 0.f;

    bool dirty = true;
    bool ready = false;
};
} // namespace eacp::UI
