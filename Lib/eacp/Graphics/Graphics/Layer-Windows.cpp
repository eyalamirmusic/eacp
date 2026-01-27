// Windows implementation of Layer base class
// On Windows, layers are purely software-emulated since there's no CALayer equivalent
#include "Layer.h"
#include "NativeLayer-Windows.h"

namespace eacp::Graphics
{

void Layer::attachToLayer(void* /*nativeLayer*/)
{
    // On Windows, layers don't attach to native views
    // Rendering is handled by Window during paint
}

void Layer::detachFromLayer()
{
    // On Windows, layers don't detach from native views
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
