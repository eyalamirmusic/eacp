#include "Layer.h"
#include "NativeLayer.h"

namespace eacp::Graphics
{


void Layer::attachTo(void* nativeLayer)
{
    auto native = (NativeLayer*) getNativeLayer();
    native->attachTo((CALayer*) nativeLayer);
}

void Layer::detachFromLayer()
{
    auto native = (NativeLayer*) getNativeLayer();
    native->detach();
}

void Layer::setBounds(const Rect& bounds)
{
    auto native = (NativeLayer*) getNativeLayer();
    native->setBounds(bounds);
}

void Layer::setPosition(const Point& position)
{
    auto native = (NativeLayer*) getNativeLayer();
    native->setPosition(position);
}

void Layer::setHidden(bool hidden)
{
    auto native = (NativeLayer*) getNativeLayer();
    native->setHidden(hidden);
}

void Layer::setOpacity(float opacity)
{
    auto native = (NativeLayer*) getNativeLayer();
    native->setOpacity(opacity);
}

} // namespace eacp::Graphics