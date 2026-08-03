#include "Layer.h"

#include "../Component/Component.h"

#include <cmath>

namespace eacp::UI
{
namespace
{
// The largest layer worth making, per side, in device pixels. A layer is a
// texture of its own rather than room in a shared atlas, so nothing here packs
// or compacts and nothing gives space back but the destructor -- which makes an
// absurd one a straightforward waste of memory rather than a ceiling anything
// negotiates. Past this the layer draws as nothing, which is visible, rather
// than as a hundred megabytes nobody asked for.
constexpr int maxLayerSize = 4096;

int devicePixels(float points, float scale)
{
    return (int) std::ceil(points * scale - 0.001f);
}
} // namespace

Layer::Layer(Component& ownerToUse)
    : owner(ownerToUse)
{
    owner.addLayer(*this);
}

Layer::~Layer()
{
    owner.removeLayer(*this);
}

void Layer::setBounds(const Rect& newBounds)
{
    if (sameRect(bounds, newBounds))
        return;

    bounds = newBounds;
    setDirty();
}

void Layer::setOpacity(float newOpacity)
{
    // Not dirty: what the layer holds is the content at full strength, and the
    // opacity is applied where it is composited. Fading one is therefore a
    // repaint and not a re-render, which is what makes a layer worth animating.
    opacity = std::clamp(newOpacity, 0.f, 1.f);
}

void Layer::setDirty()
{
    dirty = true;
    ready = false;
}

Rect Layer::getContentUV() const
{
    if (!texture.has_value() || contentWidth <= 0 || contentHeight <= 0)
        return {};

    auto width = (float) texture->width();
    auto height = (float) texture->height();

    return {0.f, 0.f, (float) contentWidth / width, (float) contentHeight / height};
}

bool Layer::ensureTexture(float scale)
{
    auto width = devicePixels(bounds.w, scale);
    auto height = devicePixels(bounds.h, scale);

    if (width <= 0 || height <= 0 || width > maxLayerSize || height > maxLayerSize)
        return false;

    contentWidth = width;
    contentHeight = height;
    renderedScale = scale;

    if (texture.has_value() && texture->width() >= width
        && texture->height() >= height)
        return true;

    auto descriptor = GPU::TextureDescriptor {};
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = GPU::TextureFormat::BGRA8Unorm;
    descriptor.renderTarget = true;

    texture.emplace(GPU::Device::shared(), descriptor, nullptr);

    if (!texture->isValid())
    {
        texture.reset();
        return false;
    }

    return true;
}

void Layer::markRendered()
{
    dirty = false;
    ready = true;
}
} // namespace eacp::UI
