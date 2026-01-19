#pragma once

#import <QuartzCore/QuartzCore.h>
#include "../Primitives/GraphicUtils.h"

namespace eacp::Graphics
{
struct NativeLayer
{
    virtual ~NativeLayer() = default;

    void attachTo(CALayer* parentLayer)
    {
        if (parentLayer && !attached)
        {
            nativeLayer.contentsScale = parentLayer.contentsScale;
            [parentLayer addSublayer:nativeLayer];
            attached = true;
        }
    }

    void detach()
    {
        if (attached)
        {
            [nativeLayer removeFromSuperlayer];
            attached = false;
        }
    }

    void setBounds(const Rect& bounds) { nativeLayer.bounds = toCGRect(bounds); }

    void setPosition(const Point& pos)
    {
        nativeLayer.position = CGPointMake(pos.x, pos.y);
    }

    void setHidden(bool hidden) { nativeLayer.hidden = hidden; }
    void setOpacity(float opacity) { nativeLayer.opacity = opacity; }

    CALayer* nativeLayer = nullptr;
    bool attached = false;
};
} // namespace eacp::Graphics