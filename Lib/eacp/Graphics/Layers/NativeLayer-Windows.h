#pragma once

#include "../DComp-Windows.h"

#include "../Primitives/Primitives.h"

namespace eacp::Graphics
{

// SetContent draws a surface 1:1 in the visual's local space, and the root is
// already scaled by the DPI factor, so content visuals counter-scale by
// 1/dpiScale. Offsets and bounds therefore stay in logical points.
struct NativeLayerBase
{
    virtual ~NativeLayerBase() = default;

    void setBounds(const Rect& boundsToUse)
    {
        if (bounds.w != boundsToUse.w || bounds.h != boundsToUse.h)
        {
            contentDirty = true;
            surfaceDirty = true;
        }

        bounds = boundsToUse;
    }

    void setPosition(const Point& pos)
    {
        position = pos;

        if (visual)
        {
            visual->SetOffsetX(position.x);
            visual->SetOffsetY(position.y);
            commitComposition();
        }
    }

    void setHidden(bool hiddenState)
    {
        hidden = hiddenState;
        applyOpacity(hiddenState ? 0.0f : opacity);
    }

    void setOpacity(float op)
    {
        opacity = op;

        if (!hidden)
            applyOpacity(opacity);
    }

    void attachTo(IDCompositionVisual2* parent)
    {
        if (!parent)
            return;

        parentVisual = parent;

        // ensureVisual() already parents when parentVisual is set, and DComp
        // reparents on a second add.
        if (!ensureVisual())
            return;

        surfaceDirty = true;
        contentDirty = true;
        positionDirty = true;
        commitComposition();
    }

    void detach()
    {
        if (parentVisual && visual)
            parentVisual->RemoveVisual(visual.Get());

        parentVisual.Reset();
        surface.Reset();
        commitComposition();
    }

    static float systemDpiScale()
    {
        auto dpi = GetDpiForSystem();
        return static_cast<float>(dpi) / 96.f;
    }

    float getDpiScale() const { return dpiScale; }

    // Pushed before each render pass by ensureAllLayersRendered; a change
    // recreates the surface at the new pixel size.
    void setDpiScale(float scale)
    {
        if (dpiScale == scale)
            return;

        dpiScale = scale;
        surfaceDirty = true;
        contentDirty = true;
    }

    virtual void createSurface()
    {
        if (!ensureVisual())
            return;

        if (bounds.w <= 0 || bounds.h <= 0)
        {
            surface.Reset();
            visual->SetContent(nullptr);
            return;
        }

        auto* device = getCompositionDevice();
        if (!device)
            return;

        auto surfaceWidth = static_cast<int>(bounds.w * dpiScale);
        auto surfaceHeight = static_cast<int>(bounds.h * dpiScale);

        surface.Reset();
        auto hr = device->CreateSurface(static_cast<UINT>(surfaceWidth),
                                        static_cast<UINT>(surfaceHeight),
                                        DXGI_FORMAT_B8G8R8A8_UNORM,
                                        DXGI_ALPHA_MODE_PREMULTIPLIED,
                                        surface.GetAddressOf());

        if (FAILED(hr))
        {
            // Keep surfaceDirty set: the post-recovery redraw retries.
            handleDeviceLossIfNeeded(hr);
            surface.Reset();
            return;
        }

        visual->SetContent(surface.Get());
        applyContentScale();

        surfaceDirty = false;
    }

    virtual void renderContent() = 0;

    // Draws into an arbitrary device context instead of this layer's own DComp
    // surface, for the off-screen snapshot. `transform` maps points -> device
    // pixels and `pointScale` is the factor baked into it, for stroke widths.
    virtual void drawInto(ID2D1DeviceContext*, const D2D1::Matrix3x2F&, float) {}

    void updateVisualPosition()
    {
        if (visual && positionDirty)
        {
            visual->SetOffsetX(position.x);
            visual->SetOffsetY(position.y);
            positionDirty = false;
        }
    }

    void updateVisualOpacity()
    {
        if (visual && opacityDirty)
        {
            applyOpacity(opacity);
            opacityDirty = false;
        }
    }

    void updateVisualVisibility()
    {
        if (visual && visibilityDirty)
        {
            applyOpacity(hidden ? 0.0f : opacity);
            visibilityDirty = false;
        }
    }

    void ensureContent()
    {
        if (surfaceDirty)
            createSurface();
        if (contentDirty && surface)
        {
            renderContent();
            contentDirty = false;
        }

        updateVisualPosition();
        updateVisualOpacity();
        updateVisualVisibility();
    }

    void markContentDirty() { contentDirty = true; }

    // Creates the visual on first use and rebuilds it when a device loss moved
    // the generation; ensureContent re-derives everything downstream.
    bool ensureVisual()
    {
        auto current = getCompositionGeneration();

        if (generation != current)
        {
            generation = current;
            visual.Reset();
            visual3.Reset();
            surface.Reset();
            surfaceDirty = true;
            contentDirty = true;
            positionDirty = true;
        }

        if (visual)
            return true;

        auto* device = getCompositionDevice();
        if (!device)
            return false;

        if (FAILED(device->CreateVisual(visual.GetAddressOf())))
        {
            visual.Reset();
            return false;
        }

        // Opacity lives on IDCompositionVisual3; the QI is cached.
        visual.As(&visual3);

        if (parentVisual)
            insertVisualAtTop(parentVisual.Get(), visual.Get());

        return true;
    }

    void applyOpacity(float value)
    {
        if (!visual3)
            return;

        visual3->SetOpacity(value);
        commitComposition();
    }

    // Undoes the root visual's DPI scale; see the note at the top of the file.
    void applyContentScale()
    {
        if (!visual || dpiScale <= 0.f)
            return;

        visual->SetTransform(
            D2D1::Matrix3x2F::Scale(1.f / dpiScale, 1.f / dpiScale));
    }

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
