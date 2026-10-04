#pragma once

#include "../DComp-Windows.h"

#include "../Primitives/Primitives.h"

namespace eacp::Graphics
{

// DComp has no SpriteVisual/ContainerVisual split — one visual type both holds
// content and parents children — and no surface *brush*: a surface is set as the
// visual's content directly.
//
// It also has no visual Size, and no stretch: SetContent draws the surface 1:1
// in the visual's local space. The root visual is scaled by the DPI factor (see
// CompositionHostWindow::rescaleRootVisualToDpi), so a content visual would show
// its already-DPI-sized surface scaled a second time. Counter-scaling the
// content visual by 1/dpiScale cancels that, which keeps offsets and bounds in
// logical points exactly as the WinRT backend had them — DComp applies offsets
// in parent space, so they are unaffected by the visual's own transform.
struct NativeLayerBase
{
    virtual ~NativeLayerBase() = default;

    void setBounds(const Rect& boundsToUse);
    void setPosition(const Point& pos);
    void setHidden(bool hiddenState);
    void setOpacity(float op);

    void attachTo(IDCompositionVisual2* parent);
    void detach();

    static float systemDpiScale();

    constexpr float getDpiScale() const { return dpiScale; }

    // The host window's DPI scale, pushed before each render pass by
    // CompositionHostWindow::ensureAllLayersRendered. A change (the window
    // moved to a monitor with different scaling) recreates the surface at
    // the new pixel size.
    void setDpiScale(float scale);

    virtual void createSurface();

    virtual void renderContent() = 0;

    // Draws this layer's content straight into an arbitrary device context under
    // `transform` (points -> device pixels), instead of into its own DComp
    // surface. This is the off-screen View->Image snapshot's analog of macOS
    // renderInContext:, which cannot reach a DComp visual. `pointScale` is the
    // point-to-pixel factor baked into `transform`, so stroke widths (specified
    // in points) match the on-screen surface. Default draws nothing.
    virtual void drawInto(ID2D1DeviceContext*, const D2D1::Matrix3x2F&, float);

    void updateVisualPosition();
    void updateVisualOpacity();
    void updateVisualVisibility();

    void ensureContent();

    constexpr void markContentDirty() { contentDirty = true; }

    // Rebuilds the visual after a device loss moved the generation, and creates
    // it on first use. Everything downstream (surface, content, position) is
    // re-derived by ensureContent, so dropping them here is enough.
    bool ensureVisual();

    void applyOpacity(float value);

    // Undoes the root visual's DPI scale for this visual's physical-pixel
    // surface — see the note at the top of the struct.
    void applyContentScale();

    Rect bounds;
    Point position;
    float opacity = 1.0f;
    float dpiScale = systemDpiScale();
    bool hidden = false;

    ComPtr<IDCompositionVisual2> visual;
    ComPtr<IDCompositionVisual3> visual3;
    ComPtr<IDCompositionSurface> surface;
    ComPtr<IDCompositionVisual2> parentVisual;
    uint64_t generation = 0;

    bool contentDirty = true;
    bool surfaceDirty = true;
    bool positionDirty = true;
    bool opacityDirty = true;
    bool visibilityDirty = false;
};

} // namespace eacp::Graphics
