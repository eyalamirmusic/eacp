#include "Layer.h"
#include "ImmediateLayerClass.h"
#include "NativeLayer.h"
#include "../Primitives/GraphicUtils.h"

namespace eacp::Graphics
{

id<CAAction> immediateActionForKey(id, SEL, NSString*)
{
    return (id<CAAction>) [NSNull null];
}

void NativeLayer::attachTo(CALayer* parentLayer)
{
    if (parentLayer && !attached)
    {
        nativeLayer.contentsScale = parentLayer.contentsScale;
        [parentLayer addSublayer:nativeLayer];
        attached = true;
    }
}

void NativeLayer::detach()
{
    if (attached)
    {
        [nativeLayer removeFromSuperlayer];
        attached = false;
    }
}

void NativeLayer::setBounds(const Rect& bounds)
{
    nativeLayer.bounds = toCGRect(bounds);
}

void NativeLayer::setPosition(const Point& pos)
{
    nativeLayer.position = CGPointMake(pos.x, pos.y);
}

void NativeLayer::setHidden(bool hidden)
{
    nativeLayer.hidden = hidden;
}

void NativeLayer::setOpacity(float opacity)
{
    nativeLayer.opacity = opacity;
}

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
