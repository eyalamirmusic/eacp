#pragma once

#import <QuartzCore/QuartzCore.h>

namespace eacp::Graphics
{
struct MacLayer
{
    virtual ~MacLayer() = default;

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

    CALayer* nativeLayer = nullptr;
    bool attached = false;
};
} // namespace eacp::Graphics