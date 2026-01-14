
#import <QuartzCore/QuartzCore.h>
#include "ShapeLayer.h"
#include "MacGraphicUtils.h"
#include "../ObjC/ObjC.h"
#include "../ObjC/CFRef.h"

namespace eacp::Graphics
{

static CFRef<CGColorRef> toCGColor(const Color& c)
{
    CGFloat components[4] = {c.r, c.g, c.b, c.a};
    auto colorSpace = CFRef<CGColorSpaceRef>(CGColorSpaceCreateDeviceRGB());
    return {CGColorCreate(colorSpace, components)};
}

struct CATransactionAction
{
    CATransactionAction()
    {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
    }

    ~CATransactionAction() { [CATransaction commit]; }
};

struct ShapeLayer::Native
{
    Native()
    {
        layer = [CAShapeLayer layer];
        layer.get().fillColor = nil;
        layer.get().strokeColor = nil;
        layer.get().lineWidth = 1.0f;
        layer.get().anchorPoint = CGPointMake(0, 0);
    }

    void setPath(CGPathRef path)
    {
        auto action = CATransactionAction();
        layer.get().path = path;
    }

    void clearPath()
    {
        auto action = CATransactionAction();
        layer.get().path = nil;
    }

    void setFillColor(const Color& color)
    {
        auto action = CATransactionAction();
        auto cgColor = toCGColor(color);
        layer.get().fillColor = cgColor;
    }

    void setStrokeColor(const Color& color)
    {
        auto action = CATransactionAction();
        auto cgColor = toCGColor(color);
        layer.get().strokeColor = cgColor;
    }

    void setStrokeWidth(float width)
    {
        auto action = CATransactionAction();
        layer.get().lineWidth = width;
    }

    void setBounds(const Rect& bounds)
    {
        auto action = CATransactionAction();
        layer.get().bounds = toCGRect(bounds);
    }

    void setPosition(const Point& pos)
    {
        auto action = CATransactionAction();
        layer.get().position = CGPointMake(pos.x, pos.y);
    }

    void setHidden(bool hidden)
    {
        auto action = CATransactionAction();
        layer.get().hidden = hidden;
    }

    void setOpacity(float opacity)
    {
        auto action = CATransactionAction();
        layer.get().opacity = opacity;
    }

    void attachTo(CALayer* parentLayer)
    {
        if (parentLayer && !attached)
        {
            layer.get().contentsScale = parentLayer.contentsScale;
            [parentLayer addSublayer:layer.get()];
            attached = true;
        }
    }

    void detach()
    {
        if (attached)
        {
            [layer.get() removeFromSuperlayer];
            attached = false;
        }
    }

    ObjC::Ptr<CAShapeLayer> layer;
    bool attached = false;
};

ShapeLayer::ShapeLayer()
    : impl()
{
}

ShapeLayer::~ShapeLayer() = default;

void ShapeLayer::setPath(const Path& path)
{
    impl->setPath((CGPathRef) path.getHandle());
}

void ShapeLayer::clearPath()
{
    impl->clearPath();
}

void ShapeLayer::setFillColor(const Color& color)
{
    impl->setFillColor(color);
}

void ShapeLayer::setStrokeColor(const Color& color)
{
    impl->setStrokeColor(color);
}

void ShapeLayer::setStrokeWidth(float width)
{
    impl->setStrokeWidth(width);
}

void ShapeLayer::setBounds(const Rect& bounds)
{
    impl->setBounds(bounds);
}

void ShapeLayer::setPosition(const Point& position)
{
    impl->setPosition(position);
}

void ShapeLayer::setHidden(bool hidden)
{
    impl->setHidden(hidden);
}

void ShapeLayer::setOpacity(float opacity)
{
    impl->setOpacity(opacity);
}

void ShapeLayer::attachToLayer(void* nativeLayer)
{
    impl->attachTo((__bridge CALayer*) nativeLayer);
}

void ShapeLayer::detachFromLayer()
{
    impl->detach();
}

} // namespace eacp::Graphics
