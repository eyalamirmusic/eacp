#include "NativeLayer-Windows.h"

namespace eacp::Graphics
{
void NativeLayerBase::setBounds(const Rect& boundsToUse)
{
    if (bounds.w != boundsToUse.w || bounds.h != boundsToUse.h)
    {
        contentDirty = true;
        surfaceDirty = true;
    }

    bounds = boundsToUse;
}

void NativeLayerBase::setPosition(const Point& pos)
{
    position = pos;

    if (visual)
    {
        visual->SetOffsetX(position.x);
        visual->SetOffsetY(position.y);
        commitComposition();
    }
}

void NativeLayerBase::setHidden(bool hiddenState)
{
    hidden = hiddenState;
    applyOpacity(hiddenState ? 0.0f : opacity);
}

void NativeLayerBase::setOpacity(float op)
{
    opacity = op;

    if (!hidden)
        applyOpacity(opacity);
}

void NativeLayerBase::attachTo(IDCompositionVisual2* parent)
{
    if (!parent)
        return;

    parentVisual = parent;

    // ensureVisual() parents the visual itself when parentVisual is already
    // set, so do not AddVisual again here — DComp reparents on a second add.
    if (!ensureVisual())
        return;

    surfaceDirty = true;
    contentDirty = true;
    positionDirty = true;
    commitComposition();
}

void NativeLayerBase::detach()
{
    if (parentVisual && visual)
        parentVisual->RemoveVisual(visual.Get());

    parentVisual.Reset();
    surface.Reset();
    commitComposition();
}

float NativeLayerBase::systemDpiScale()
{
    auto dpi = GetDpiForSystem();
    return static_cast<float>(dpi) / 96.f;
}

void NativeLayerBase::setDpiScale(float scale)
{
    if (dpiScale == scale)
        return;

    dpiScale = scale;
    surfaceDirty = true;
    contentDirty = true;
}

void NativeLayerBase::createSurface()
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

void NativeLayerBase::drawInto(ID2D1DeviceContext*, const D2D1::Matrix3x2F&, float)
{
}

void NativeLayerBase::updateVisualPosition()
{
    if (visual && positionDirty)
    {
        visual->SetOffsetX(position.x);
        visual->SetOffsetY(position.y);
        positionDirty = false;
    }
}

void NativeLayerBase::updateVisualOpacity()
{
    if (visual && opacityDirty)
    {
        applyOpacity(opacity);
        opacityDirty = false;
    }
}

void NativeLayerBase::updateVisualVisibility()
{
    if (visual && visibilityDirty)
    {
        applyOpacity(hidden ? 0.0f : opacity);
        visibilityDirty = false;
    }
}

void NativeLayerBase::ensureContent()
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

bool NativeLayerBase::ensureVisual()
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

    // Opacity lives on IDCompositionVisual3; the QI is done once and cached.
    visual.As(&visual3);

    if (parentVisual)
        insertVisualAtTop(parentVisual.Get(), visual.Get());

    return true;
}

void NativeLayerBase::applyOpacity(float value)
{
    if (!visual3)
        return;

    visual3->SetOpacity(value);
    commitComposition();
}

void NativeLayerBase::applyContentScale()
{
    if (!visual || dpiScale <= 0.f)
        return;

    visual->SetTransform(D2D1::Matrix3x2F::Scale(1.f / dpiScale, 1.f / dpiScale));
}
} // namespace eacp::Graphics
