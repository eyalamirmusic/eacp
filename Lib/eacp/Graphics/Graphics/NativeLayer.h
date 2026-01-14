#pragma once

#import <QuartzCore/QuartzCore.h>

namespace eacp::Graphics
{
struct MacLayer
{
    virtual ~MacLayer() { detach(); }

    virtual CALayer* getLayer() = 0;

    void attachTo(CALayer* parentLayer)
    {
        if (parentLayer && !attached)
        {
            auto layer = getLayer();
            layer.contentsScale = parentLayer.contentsScale;
            [parentLayer addSublayer:layer];
            attached = true;
        }
    }

    void detach()
    {
        if (attached)
        {
            [getLayer() removeFromSuperlayer];
            attached = false;
        }
    }

    bool attached = false;
};
} // namespace eacp::Graphics