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
} // namespace eacp::Graphics