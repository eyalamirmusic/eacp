#pragma once

#include "../Primitives/Primitives.h"

#define NOMINMAX
#include <Windows.h>
#include <d2d1_1.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <windows.ui.composition.interop.h>

namespace wuc = winrt::Windows::UI::Composition;
namespace wgdx = winrt::Windows::Graphics::DirectX;

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

// Forward declarations
wuc::Compositor getWinRTCompositor();
wuc::CompositionGraphicsDevice getCompositionGraphicsDevice();
ID2D1Device* getD2DDevice();

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
        positionDirty = true;
    }

    void setHidden(bool hiddenState)
    {
        hidden = hiddenState;
        visibilityDirty = true;
    }

    void setOpacity(float op)
    {
        opacity = op;
        opacityDirty = true;
    }

    // Create the visual and attach to parent
    void attachTo(wuc::ContainerVisual parent)
    {
        if (!parent)
            return;

        parentVisual = parent;

        auto compositor = getWinRTCompositor();
        if (!compositor)
            return;

        // Create SpriteVisual for this layer if not already created
        if (!visual)
        {
            visual = compositor.CreateSpriteVisual();
        }

        if (visual)
        {
            // Add visual to parent's children
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

    // Create/recreate surface at current bounds size
    virtual void createSurface()
    {
        if (!visual)
            return;

        auto graphicsDevice = getCompositionGraphicsDevice();
        auto compositor = getWinRTCompositor();
        if (!graphicsDevice || !compositor)
            return;

        // Calculate surface size in pixels (apply DPI scaling if needed)
        int width = static_cast<int>(bounds.w);
        int height = static_cast<int>(bounds.h);

        if (width <= 0 || height <= 0)
        {
            surface = nullptr;
            surfaceBrush = nullptr;
            visual.Brush(nullptr);
            return;
        }

        // Create drawing surface
        surface = graphicsDevice.CreateDrawingSurface(
            {static_cast<float>(width), static_cast<float>(height)},
            wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            wgdx::DirectXAlphaMode::Premultiplied);

        if (surface)
        {
            // Create surface brush and set on visual
            surfaceBrush = compositor.CreateSurfaceBrush(surface);
            visual.Brush(surfaceBrush);
            visual.Size({static_cast<float>(width), static_cast<float>(height)});
        }

        surfaceDirty = false;
    }

    // Render content to surface - implemented by derived classes
    virtual void renderContent() = 0;

    // Update visual position
    void updateVisualPosition()
    {
        if (visual && positionDirty)
        {
            visual.Offset({position.x, position.y, 0.0f});
            positionDirty = false;
        }
    }

    // Update visual opacity
    void updateVisualOpacity()
    {
        if (visual && opacityDirty)
        {
            visual.Opacity(opacity);
            opacityDirty = false;
        }
    }

    // Update visual visibility
    void updateVisualVisibility()
    {
        if (visual && visibilityDirty)
        {
            // Use opacity 0 for hidden
            if (hidden)
            {
                visual.Opacity(0.0f);
            }
            else
            {
                visual.Opacity(opacity);
            }
            visibilityDirty = false;
        }
    }

    // Ensure content is rendered if dirty
    void ensureContent()
    {
        if (surfaceDirty)
        {
            createSurface();
        }
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

    // Basic properties
    Rect bounds;
    Point position;
    float opacity = 1.0f;
    bool hidden = false;

    // Windows.UI.Composition objects
    wuc::SpriteVisual visual{nullptr};
    wuc::CompositionDrawingSurface surface{nullptr};
    wuc::CompositionSurfaceBrush surfaceBrush{nullptr};
    wuc::ContainerVisual parentVisual{nullptr};

    // Dirty tracking
    bool contentDirty = true;
    bool surfaceDirty = true;
    bool positionDirty = true;
    bool opacityDirty = true;
    bool visibilityDirty = false;
};

} // namespace eacp::Graphics
