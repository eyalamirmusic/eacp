#pragma once

#include "../Common.h"

#include <eacp/GPU/GPU.h>
#include <eacp/GPUWidgets/Path/AffineTransform.h>

#include <functional>
#include <optional>

namespace eacp::UI
{
class Component;
class GPUComponent;
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
//
// What fills it is one of three things, the first that is set winning:
// onRender, a GPU pass of the caller's own; setContent, a subtree of components
// drawn into it in place of the frame; and onPaint. Whichever it is, the result
// is composited the same way, by drawLayer or by sampling getTexture().
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
    Rect getBounds() const;

    // Multiplied into the whole layer as it is composited, which is the point of
    // having one.
    void setOpacity(float newOpacity);
    float getOpacity() const;

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
    const GPUWidgets::AffineTransform& getTransform() const;

    // What goes in it, drawn with the origin at the layer's own top-left -- so a
    // caller draws in the same coordinates it would have drawn in without a
    // layer, and moving the layer moves the content with it.
    //
    // Called by the host, before the frame's pass, and only when the layer is
    // dirty. Non-null by default so nothing has to check.
    std::function<void(Graphics&)> onPaint = [](Graphics&) {};

    // A subtree of the owner drawn into this layer instead of into the frame:
    // the frame's own pass steps over it, and the owner puts it on screen --
    // with drawLayer, or by sampling the texture in a pass of its own. Null,
    // the default, clears it.
    //
    // Normally a direct child of the owner, and always a descendant. It stays
    // an ordinary part of the tree in every other respect: laid out, its paths
    // rasterized, its own layers rendered (before this one, being children),
    // its timers and listeners running, and found by getComponentAt as any
    // visible component is. Only the frame's drawing of it moves here.
    //
    // A point at the content's position in the owner lands at that position
    // less getBounds()'s corner in the texture, so bounds covering the content
    // where it sits draw it 1:1. Cut to the bounds; ancestors between the owner
    // and the content do not clip it.
    //
    // Rendered again whenever anything in the subtree changes what the frame
    // would have drawn -- a repaint, a move or resize, a child shown, hidden,
    // added, removed or reordered, a layer inside it rendered or faded -- and
    // not otherwise. onPaint is not called while this is set.
    //
    // Let go of, rather than left dangling, when the content is deleted or
    // removed from its parent; it then has to be set again.
    void setContent(Component* subtreeRoot);
    Component* getContent() const;

    // A pass of the caller's own instead of drawn content: a 3D scene with a
    // depth buffer, a projection of another layer, anything the tier's 2D
    // renderers cannot express. Empty by default; while set, the layer is
    // filled by this alone and both setContent and onPaint are ignored.
    //
    // The host opens a pass on the texture -- cleared to transparent, with the
    // depth buffer cleared when hasDepth() -- sets its viewport to
    // `contentAreaPixels`, the run of the texture the layer occupies in device
    // pixels from its top-left, and calls this. Nothing else goes into it.
    // The pipelines drawn here are built to match the texture:
    //
    //     descriptor.colorFormat = layer.getColorFormat();
    //     descriptor.depth = layer.hasDepth();
    //     descriptor.sampleCount = layer.getSampleCount();
    //
    // The texture is composited as any layer is, opacity and transform
    // included, and the compositor treats it as drawn onto transparent -- so a
    // pipeline that is not opaque blends with
    // GPU::BlendMode::AlphaBlendOntoTransparent, as the tier's own do.
    //
    // **Sampling another layer works by the ordering rule above.** A layer
    // registered earlier on the same component is rendered first, so this one
    // can sample its texture; and whenever one of those is rendered again this
    // one is too, in the same frame, with nothing to wire up. For a change of
    // its own -- a camera moving -- call setDirty. Assigning this does not mark
    // the layer dirty either.
    std::function<void(GPU::RenderPass& pass, const Rect& contentAreaPixels)>
        onRender;

    // A depth buffer on the texture, for an onRender pass drawing a scene.
    // Ignored for a layer without onRender, whose content has no use for one.
    // Changing it remakes the texture.
    void setDepth(bool shouldHaveDepth);
    bool hasDepth() const;

    // A stencil plane beside that depth buffer, for pipelines setting
    // RenderPipelineDescriptor::stencil. Brings the depth buffer with it, the
    // two planes being one attachment; ignored without onRender, and changing
    // it remakes the texture.
    void setStencil(bool shouldHaveStencil);
    bool hasStencil() const;

    // Samples per pixel for an onRender pass. Above 1 the pass draws into a
    // multisampled twin and is resolved into the texture as it ends, so what
    // is composited and sampled is always the resolved picture. A count the
    // device refuses leaves the layer empty -- GPU::Device::supportsSampleCount
    // is how to choose one. Ignored without onRender; changing it remakes the
    // texture.
    void setSampleCount(int newSampleCount);
    int getSampleCount() const;

    // The texture's colour format, as a pipeline drawing into it names it.
    GPU::PixelFormat getColorFormat() const;

    // Asks for the content to be rendered again at the top of the next frame.
    // Setting the bounds does this for you; a change of what onPaint would draw
    // does not, since nothing here can see it.
    void setDirty();

    // True once there is a texture with the content in it. Drawing an empty
    // layer is a no-op.
    bool isEmpty() const;

    const GPU::Texture& getTexture() const;

    // The part of that texture the content occupies, in normalised coordinates.
    // Not the whole of it: the texture is kept across a shrink, so a layer that
    // got smaller draws out of the corner it filled rather than reallocating.
    Rect getContentUV() const;

private:
    friend class ComponentHost;
    friend class Component;
    friend class GPUComponent;
    friend class LayerRenderer;

    // Something in the captured subtree changed what it draws. Only the flag:
    // the owner's recording names the layer rather than its pixels, so it
    // still holds, and the change that called this asks for the frame itself.
    void contentChanged();

    // The bounds a frame layer is kept at, set by the host's walk while the
    // frame is being built. Marks the layer and the owner's drawing stale
    // without asking for another frame, since the one in progress is about to
    // render and record both.
    void syncFrameBounds(const Rect& newBounds);

    bool rendersOwnPass() const;
    bool rendersFrame() const;
    bool wantsDepth() const;
    bool wantsStencil() const;
    int wantedSampleCount() const;

    // Dirty, or last rendered at another scale -- the texture is in device
    // pixels, so content drawn for one display is the wrong resolution on the next.
    bool needsRendering(float scale) const;

    // The top-left run of the texture the content was rendered into, in texels:
    // what a pass filling the layer has to confine itself to, since the texture
    // may be larger than the content after a shrink.
    Rect getContentArea() const;

    // Makes or grows the texture for the bounds at this scale. False when the
    // bounds hold no pixels, or the device could not give a texture -- either
    // way the layer draws as nothing. Kept between renders and only remade when
    // it is too small, so a layer redrawn every frame allocates nothing.
    bool ensureTexture(float scale);
    void reportTooLarge(int width, int height);

    // The content has just been rendered into the texture, at the scale
    // ensureTexture was given.
    void markRendered();

    Component& owner;
    Component* content = nullptr;

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

    bool depth = false;
    bool stencil = false;
    int sampleCount = 1;

    bool dirty = true;
    bool ready = false;

    // Composited as a solid rectangle, the texture's alpha ignored, rather than
    // as colour weighted by its coverage.
    bool opaque = false;

    // Said once rather than every frame, for a layer the device would not give
    // a texture of its size.
    bool reportedTooLarge = false;

    // The component filling this layer with a whole redirected GPU::Frame
    // rather than a pass the host opens, with a texture of exactly the owner's
    // pixels, since the frame's code sizes itself off the target. Set by
    // GPUComponent, which is also the owner.
    GPUComponent* frameComponent = nullptr;
};
} // namespace eacp::UI
