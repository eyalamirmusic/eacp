// Windows implementation of Layer base class using DirectComposition
#include "Layer.h"
#include "NativeLayer-Windows.h"

#include <dcomp.h>

namespace eacp::Graphics
{

void Layer::attachToLayer(void* parentVisual)
{
    auto native = static_cast<NativeLayerBase*>(getNativeLayer());
    if (native && parentVisual)
    {
        native->attachTo(static_cast<IDCompositionVisual2*>(parentVisual));
    }
}

void Layer::detachFromLayer()
{
    auto native = static_cast<NativeLayerBase*>(getNativeLayer());
    if (native)
    {
        native->detach();
    }
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
