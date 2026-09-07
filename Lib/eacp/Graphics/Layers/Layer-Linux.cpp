#include "Layer.h"

// No Layer subclass is built on Linux, so these exist only so View::addLayer
// links; none of them is reachable.

namespace eacp::Graphics
{
void Layer::attachTo(void*) {}
void Layer::detachFromLayer() {}
void Layer::setBounds(const Rect&) {}
void Layer::setPosition(const Point&) {}
void Layer::setHidden(bool) {}
void Layer::setOpacity(float) {}
} // namespace eacp::Graphics
