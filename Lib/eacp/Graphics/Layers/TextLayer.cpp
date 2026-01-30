#include "TextLayer.h"

namespace eacp::Graphics
{
TextLayer::~TextLayer()
{
    detachFromView();
}
}