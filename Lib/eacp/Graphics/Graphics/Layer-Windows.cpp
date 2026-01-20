// Windows implementation of Layer base class
// On Windows, layers are purely software-emulated since there's no CALayer equivalent
#include "Layer.h"

#include <cassert>

namespace eacp::Graphics
{

// Layer base class methods - these are stubs that track state
// The actual rendering happens in View when it paints

static Rect layerBounds;
static Point layerPosition;
static bool layerHidden = false;
static float layerOpacity = 1.0f;
static void* parentLayer = nullptr;

void Layer::attachToLayer(void* nativeLayer)
{
    parentLayer = nativeLayer;
}

void Layer::detachFromLayer()
{
    parentLayer = nullptr;
}

void Layer::setBounds(const Rect& bounds)
{
    // Store bounds - actual rendering uses these when View paints
    // Each layer subclass stores its own bounds in its Native struct
}

void Layer::setPosition(const Point& position)
{
    // Store position - actual rendering uses these when View paints
}

void Layer::setHidden(bool hidden)
{
    // Store hidden state
}

void Layer::setOpacity(float opacity)
{
    // Store opacity
}

} // namespace eacp::Graphics
