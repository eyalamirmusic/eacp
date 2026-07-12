#include "Layer.h"
#include "NativeLayer-Windows.h"

namespace eacp::Graphics
{

void Layer::attachTo(void* parentVisualPtr)
{
    auto native = static_cast<NativeLayerBase*>(getNativeLayer());

    if (native && parentVisualPtr)
        native->attachTo(static_cast<IDCompositionVisual2*>(parentVisualPtr));
}

void Layer::detachFromLayer()
{
    auto native = static_cast<NativeLayerBase*>(getNativeLayer());

    if (native)
        native->detach();
}

void Layer::setBounds(const Rect& bounds)
{
    auto native = static_cast<NativeLayerBase*>(getNativeLayer());
    native->setBounds(bounds);
}

void Layer::setPosition(const Point& position)
{
    auto native = static_cast<NativeLayerBase*>(getNativeLayer());
    native->setPosition(position);
}

void Layer::setHidden(bool hidden)
{
    auto native = static_cast<NativeLayerBase*>(getNativeLayer());
    native->setHidden(hidden);
}

void Layer::setOpacity(float opacity)
{
    auto native = static_cast<NativeLayerBase*>(getNativeLayer());
    native->setOpacity(opacity);
}

} // namespace eacp::Graphics
