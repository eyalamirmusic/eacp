#pragma once

#include "../Primitives/Primitives.h"

namespace eacp::Graphics
{
struct Layer
{
    virtual ~Layer() = default;

    void attachToLayer(void* nativeLayer);
    void detachFromLayer();

    virtual void* getNativeLayer() = 0;
};
} // namespace eacp::Graphics
