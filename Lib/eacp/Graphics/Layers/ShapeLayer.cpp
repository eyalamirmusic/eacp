#include "ShapeLayer.h"

namespace eacp::Graphics
{
ShapeLayer::~ShapeLayer()
{
    detachFromView();
}
}