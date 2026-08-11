#include "Layer.h"

#include "../Component/Component.h"

#include <cmath>

namespace eacp::UI
{
namespace
{
// Per side, in device pixels. Past this the layer draws as nothing.
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

    // The owner's recording holds a pointer to this layer.
    owner.repaint();
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
    // Neither dirty nor a repaint: the texture holds the content at full
    // strength, and the opacity is read off the layer as it is composited.
    opacity = std::clamp(newOpacity, 0.f, 1.f);
    owner.invalidateHost();
}

void Layer::setDirty()
{
    dirty = true;
    ready = false;

    // The owner recorded this layer as a quad of its bounds.
    owner.repaint();
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
