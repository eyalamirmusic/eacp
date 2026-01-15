#include "Layer.h"
#include "NativeLayer.h"

namespace eacp::Graphics
{

void Layer::attachToLayer(void* nativeLayer)
{
    auto native = (MacLayer*) getNativeLayer();
    native->attachTo((CALayer*) nativeLayer);
}

void Layer::detachFromLayer()
{
    auto native = (MacLayer*) getNativeLayer();
    native->detach();
}

void Layer::setBounds(const Rect& bounds)
{
    auto native = (MacLayer*) getNativeLayer();
    native->setBounds(bounds);
}

void Layer::setPosition(const Point& position)
{
    auto native = (MacLayer*) getNativeLayer();
    native->setPosition(position);
}

void Layer::setHidden(bool hidden)
{
    auto native = (MacLayer*) getNativeLayer();
    native->setHidden(hidden);
}

void Layer::setOpacity(float opacity)
{
    auto native = (MacLayer*) getNativeLayer();
    native->setOpacity(opacity);
}

} // namespace eacp::Graphics