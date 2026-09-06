#include "Layer.h"

// Layer's platform half on Linux, which exists so View::addLayer links and for
// no other reason.
//
// Layer is abstract — getNativeLayer() is pure virtual — and the two classes
// that implement it, ShapeLayer and TextLayer, are Core Animation and Direct2D
// and are not built here. So no Layer object can be constructed on Linux, and
// every function below is unreachable rather than merely unimplemented. They
// are definitions, not stubs waiting to grow: the retained-layer tier is a
// question for whatever 2D backend Linux eventually gets (plan §6), and if that
// turns out to be the GPU coverage rasterizer rather than a Cairo, this file
// disappears instead of filling in.

namespace eacp::Graphics
{
void Layer::attachTo(void*) {}
void Layer::detachFromLayer() {}
void Layer::setBounds(const Rect&) {}
void Layer::setPosition(const Point&) {}
void Layer::setHidden(bool) {}
void Layer::setOpacity(float) {}
} // namespace eacp::Graphics
