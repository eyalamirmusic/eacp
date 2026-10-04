#pragma once

#import <QuartzCore/QuartzCore.h>
#include "../Primitives/Primitives.h"

namespace eacp::Graphics
{
struct NativeLayer
{
    virtual ~NativeLayer() = default;

    void attachTo(CALayer* parentLayer);
    void detach();

    void setBounds(const Rect& bounds);
    void setPosition(const Point& pos);

    void setHidden(bool hidden);
    void setOpacity(float opacity);

    CALayer* nativeLayer = nullptr;
    bool attached = false;
};
} // namespace eacp::Graphics
