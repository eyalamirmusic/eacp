#include "Layer.h"

#include "../Component/Component.h"

#include <eacp/Core/Utils/Logging.h>

#include <algorithm>
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

// A GPUComponent's texture is exactly its pixels, so the ceiling is a display
// rather than a drawing: 8192 covers a full-screen component on a 5K panel. A
// device that cannot make one that large is answered the same way, by drawing
// nothing and saying so once.
constexpr int maxFrameLayerSize = 8192;

constexpr auto colorFormat = GPU::TextureFormat::BGRA8Unorm;

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

    if (content != nullptr)
        content->capturingLayer = nullptr;

    // Whatever drew it is holding a pointer to it. The owner is the component
    // that draws a layer -- that is the arrangement the class documents -- so
    // dropping its recording here is what stops the next frame replaying a quad
    // of a texture that has gone.
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
    // Not dirty: what the layer holds is the content at full strength, and the
    // opacity is applied where it is composited. Fading one is therefore a
    // repaint and not a re-render, which is what makes a layer worth animating.
    opacity = std::clamp(newOpacity, 0.f, 1.f);

    // And not a paint either: the opacity is read off the layer as it is drawn,
    // so what the owner recorded still holds and only the frame has to happen.
    owner.invalidateHost();
}

void Layer::setTransform(const GPUWidgets::AffineTransform& newTransform)
{
    // As with the opacity, and for the same reason: the content is drawn once
    // and placed by four corners at composite time, so turning it is a frame
    // and not a pass.
    transform = newTransform;
    owner.invalidateHost();
}

void Layer::setContent(Component* subtreeRoot)
{
    if (subtreeRoot == &owner)
        subtreeRoot = nullptr;

    if (content == subtreeRoot)
        return;

    if (content != nullptr)
        content->capturingLayer = nullptr;

    // One layer to a subtree: the frame steps over a captured one, so two
    // claiming it would leave both drawing what only one can be told about.
    if (subtreeRoot != nullptr && subtreeRoot->capturingLayer != nullptr)
        subtreeRoot->capturingLayer->setContent(nullptr);

    content = subtreeRoot;

    if (content != nullptr)
        content->capturingLayer = this;

    setDirty();
}

Component* Layer::getContent() const
{
    return content;
}

void Layer::setDepth(bool shouldHaveDepth)
{
    if (depth == shouldHaveDepth)
        return;

    depth = shouldHaveDepth;
    setDirty();
}

bool Layer::hasDepth() const
{
    return depth;
}

void Layer::setStencil(bool shouldHaveStencil)
{
    if (stencil == shouldHaveStencil)
        return;

    stencil = shouldHaveStencil;
    setDirty();
}

bool Layer::hasStencil() const
{
    return stencil;
}

void Layer::setSampleCount(int newSampleCount)
{
    newSampleCount = std::max(1, newSampleCount);

    if (sampleCount == newSampleCount)
        return;

    sampleCount = newSampleCount;
    setDirty();
}

int Layer::getSampleCount() const
{
    return sampleCount;
}

GPU::PixelFormat Layer::getColorFormat() const
{
    return GPU::pixelFormatFor(colorFormat);
}

void Layer::contentChanged()
{
    dirty = true;
}

void Layer::syncFrameBounds(const Rect& newBounds)
{
    if (sameRect(bounds, newBounds))
        return;

    bounds = newBounds;
    dirty = true;
    ready = false;

    // The owner drew the layer as a quad of its old bounds. Recorded later in
    // this same frame, so no frame has to be asked for.
    if (!owner.selfDirty)
    {
        owner.selfDirty = true;
        owner.markAncestorsDirty();
    }
}

bool Layer::rendersOwnPass() const
{
    return frameComponent != nullptr || static_cast<bool>(onRender);
}

bool Layer::rendersFrame() const
{
    return frameComponent != nullptr;
}

bool Layer::wantsDepth() const
{
    return rendersOwnPass() && (depth || stencil);
}

bool Layer::wantsStencil() const
{
    return rendersOwnPass() && stencil;
}

int Layer::wantedSampleCount() const
{
    return rendersOwnPass() ? sampleCount : 1;
}

void Layer::setDirty()
{
    dirty = true;
    ready = false;

    // The owner drew this layer as a quad of its bounds, so a change of what is
    // in it -- or of where it is -- is a change to what the owner recorded.
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
    auto largest = rendersFrame() ? maxFrameLayerSize : maxLayerSize;

    if (width <= 0 || height <= 0)
        return false;

    if (width > largest || height > largest)
    {
        reportTooLarge(width, height);
        texture.reset();
        ready = false;
        return false;
    }

    contentWidth = width;
    contentHeight = height;
    renderedScale = scale;

    // Remade rather than kept when it has the wrong attachments, which only a
    // layer turning into or out of an onRender one, or changing what that asks
    // for, brings about. A frame layer is remade on any change of size, shrink
    // included: the code filling it sizes itself off the whole target.
    auto fits = rendersFrame() ? texture.has_value() && texture->width() == width
                                     && texture->height() == height
                               : texture.has_value() && texture->width() >= width
                                     && texture->height() >= height;

    if (fits && texture->hasDepth() == wantsDepth()
        && texture->hasStencil() == wantsStencil()
        && texture->sampleCount() == wantedSampleCount())
        return true;

    auto descriptor = GPU::TextureDescriptor {};
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = colorFormat;
    descriptor.renderTarget = true;
    descriptor.depth = wantsDepth();
    descriptor.stencil = wantsStencil();
    descriptor.sampleCount = wantedSampleCount();

    texture.emplace(GPU::Device::shared(), descriptor, nullptr);

    if (!texture->isValid())
    {
        texture.reset();
        ready = false;
        reportTooLarge(width, height);
        return false;
    }

    reportedTooLarge = false;
    return true;
}

void Layer::reportTooLarge(int width, int height)
{
    if (reportedTooLarge)
        return;

    reportedTooLarge = true;
    LOG("eacp: a layer of ",
        width,
        "x",
        height,
        " pixels could not be given a texture and draws nothing");
}

void Layer::markRendered()
{
    dirty = false;
    ready = true;
}

Rect Layer::getBounds() const
{
    return bounds;
}

float Layer::getOpacity() const
{
    return opacity;
}

const GPUWidgets::AffineTransform& Layer::getTransform() const
{
    return transform;
}

bool Layer::isEmpty() const
{
    return !ready;
}

const GPU::Texture& Layer::getTexture() const
{
    return *texture;
}

bool Layer::needsRendering(float scale) const
{
    return dirty || scale != renderedScale;
}

Rect Layer::getContentArea() const
{
    return {0.f, 0.f, (float) contentWidth, (float) contentHeight};
}
} // namespace eacp::UI
