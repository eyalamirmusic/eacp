#pragma once

#include "../Primitives/Primitives.h"

#define NOMINMAX
#include <Windows.h>
#include <dcomp.h>
#include <d2d1_1.h>
#include <wrl/client.h>

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

// Forward declarations
IDCompositionDesktopDevice* getDCompDevice();
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
    void attachTo(IDCompositionVisual2* parent)
    {
        if (!parent)
            return;

        parentVisual = parent;

        auto* dcompDevice = getDCompDevice();
        if (!dcompDevice)
            return;

        // Create visual for this layer if not already created
        if (!visual)
        {
            ComPtr<IDCompositionVisual2> visual2;
            dcompDevice->CreateVisual(visual2.GetAddressOf());
            if (visual2)
            {
                // Query for IDCompositionVisual3 which has SetOpacity
                visual2.As(&visual);
            }
        }

        if (visual)
        {
            // Add visual to parent (use the base interface)
            ComPtr<IDCompositionVisual> baseVisual;
            visual.As(&baseVisual);
            if (baseVisual)
            {
                parent->AddVisual(baseVisual.Get(), TRUE, nullptr);
            }
            surfaceDirty = true;
            contentDirty = true;
            positionDirty = true;
        }
    }

    void detach()
    {
        if (parentVisual && visual)
        {
            ComPtr<IDCompositionVisual> baseVisual;
            visual.As(&baseVisual);
            if (baseVisual)
            {
                parentVisual->RemoveVisual(baseVisual.Get());
            }
        }
        parentVisual = nullptr;
        surface.Reset();
    }

    // Create/recreate surface at current bounds size
    virtual void createSurface()
    {
        if (!visual)
            return;

        auto* dcompDevice = getDCompDevice();
        if (!dcompDevice)
            return;

        // Calculate surface size in pixels (apply DPI scaling if needed)
        int width = static_cast<int>(bounds.w);
        int height = static_cast<int>(bounds.h);

        if (width <= 0 || height <= 0)
        {
            surface.Reset();
            visual->SetContent(nullptr);
            return;
        }

        // Create virtual surface
        surface.Reset();
        HRESULT hr = dcompDevice->CreateVirtualSurface(
            width, height, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
            surface.GetAddressOf());

        if (SUCCEEDED(hr) && surface)
        {
            visual->SetContent(surface.Get());
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
            visual->SetOffsetX(position.x);
            visual->SetOffsetY(position.y);
            positionDirty = false;
        }
    }

    // Update visual opacity
    void updateVisualOpacity()
    {
        if (visual && opacityDirty)
        {
            visual->SetOpacity(opacity);
            opacityDirty = false;
        }
    }

    // Update visual visibility
    void updateVisualVisibility()
    {
        if (visual && visibilityDirty)
        {
            // DirectComposition doesn't have a direct visibility property
            // We use opacity 0 for hidden, or we could remove/add the visual
            if (hidden)
            {
                visual->SetOpacity(0.0f);
            }
            else
            {
                visual->SetOpacity(opacity);
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

    // DirectComposition objects - use IDCompositionVisual3 for SetOpacity support
    ComPtr<IDCompositionVisual3> visual;
    ComPtr<IDCompositionVirtualSurface> surface;
    IDCompositionVisual2* parentVisual = nullptr;

    // Dirty tracking
    bool contentDirty = true;
    bool surfaceDirty = true;
    bool positionDirty = true;
    bool opacityDirty = true;
    bool visibilityDirty = false;
};

} // namespace eacp::Graphics
