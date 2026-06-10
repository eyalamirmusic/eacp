#pragma once

#include <eacp/Core/Utils/WinInclude.h>

#include "../Primitives/Primitives.h"

#include <d2d1_1.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.Graphics.DirectX.h>

namespace wuc = winrt::Windows::UI::Composition;
namespace wgdx = winrt::Windows::Graphics::DirectX;

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

wuc::Compositor getWinRTCompositor();
wuc::CompositionGraphicsDevice getCompositionGraphicsDevice();
ID2D1Device* getD2DDevice();

// Recovers the shared rendering device when `hr` is a device-loss HRESULT
// (DXGI_ERROR_DEVICE_REMOVED/RESET, D2DERR_RECREATE_TARGET). Returns true if
// recovery ran; the caller should drop the current frame — every host redraws
// once the replacement device is installed. Defined in D2DFactory-Windows.cpp.
bool handleDeviceLossIfNeeded(HRESULT hr);

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
            visual.Offset({position.x, position.y, 0.0f});
    }

    void setHidden(bool hiddenState)
    {
        hidden = hiddenState;

        if (visual)
            visual.Opacity(hiddenState ? 0.0f : opacity);
    }

    void setOpacity(float op)
    {
        opacity = op;

        if (visual && !hidden)
            visual.Opacity(opacity);
    }

    void attachTo(wuc::ContainerVisual parent)
    {
        if (!parent)
            return;

        parentVisual = parent;

        auto compositor = getWinRTCompositor();
        if (!compositor)
            return;

        if (!visual)
            visual = compositor.CreateSpriteVisual();

        if (visual)
        {
            parent.Children().InsertAtTop(visual);
            surfaceDirty = true;
            contentDirty = true;
            positionDirty = true;
        }
    }

    void detach()
    {
        if (parentVisual && visual)
        {
            parentVisual.Children().Remove(visual);
        }
        parentVisual = nullptr;
        surface = nullptr;
        surfaceBrush = nullptr;
    }

    static float systemDpiScale()
    {
        auto dpi = GetDpiForSystem();
        return static_cast<float>(dpi) / 96.f;
    }

    float getDpiScale() const { return dpiScale; }

    // The host window's DPI scale, pushed before each render pass by
    // CompositionHostWindow::ensureAllLayersRendered. A change (the window
    // moved to a monitor with different scaling) recreates the surface at
    // the new pixel size.
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
        if (!visual)
            return;

        auto graphicsDevice = getCompositionGraphicsDevice();
        auto compositor = getWinRTCompositor();
        if (!graphicsDevice || !compositor)
            return;

        if (bounds.w <= 0 || bounds.h <= 0)
        {
            surface = nullptr;
            surfaceBrush = nullptr;
            visual.Brush(nullptr);
            return;
        }

        auto surfaceWidth = static_cast<int>(bounds.w * dpiScale);
        auto surfaceHeight = static_cast<int>(bounds.h * dpiScale);

        try
        {
            surface = graphicsDevice.CreateDrawingSurface(
                {static_cast<float>(surfaceWidth),
                 static_cast<float>(surfaceHeight)},
                wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                wgdx::DirectXAlphaMode::Premultiplied);
        }
        catch (const winrt::hresult_error& e)
        {
            // Keep surfaceDirty set: the post-recovery redraw retries.
            handleDeviceLossIfNeeded(e.code());
            surface = nullptr;
            return;
        }

        if (surface)
        {
            surfaceBrush = compositor.CreateSurfaceBrush(surface);
            visual.Brush(surfaceBrush);
            visual.Size({bounds.w, bounds.h});
        }

        surfaceDirty = false;
    }

    virtual void renderContent() = 0;

    void updateVisualPosition()
    {
        if (visual && positionDirty)
        {
            visual.Offset({position.x, position.y, 0.0f});
            positionDirty = false;
        }
    }

    void updateVisualOpacity()
    {
        if (visual && opacityDirty)
        {
            visual.Opacity(opacity);
            opacityDirty = false;
        }
    }

    void updateVisualVisibility()
    {
        if (visual && visibilityDirty)
        {
            if (hidden)
                visual.Opacity(0.0f);
            else
                visual.Opacity(opacity);

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

    Rect bounds;
    Point position;
    float opacity = 1.0f;
    float dpiScale = systemDpiScale();
    bool hidden = false;

    wuc::SpriteVisual visual {nullptr};
    wuc::CompositionDrawingSurface surface {nullptr};
    wuc::CompositionSurfaceBrush surfaceBrush {nullptr};
    wuc::ContainerVisual parentVisual {nullptr};

    bool contentDirty = true;
    bool surfaceDirty = true;
    bool positionDirty = true;
    bool opacityDirty = true;
    bool visibilityDirty = false;
};

} // namespace eacp::Graphics
