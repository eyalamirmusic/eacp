#include "NativeLayer.h"
#include "ShapeLayer.h"

namespace eacp::Graphics
{

struct ShapeLayer::Native : public MacLayer
{
    Native()
    {
        layer = [CAShapeLayer layer];
        layer.get().fillColor = nil;
        layer.get().strokeColor = nil;
        layer.get().lineWidth = 1.0f;
        layer.get().anchorPoint = CGPointMake(0, 0);

        nativeLayer = layer.get();
    }

    ~Native() override { detach(); }

    void setPath(CGPathRef path) { layer.get().path = path; }

    void clearPath() { layer.get().path = nil; }

    void setFillColor(const Color& color)
    {
        auto cgColor = toCGColor(color);
        layer.get().fillColor = cgColor;
    }

    void setStrokeColor(const Color& color)
    {
        auto cgColor = toCGColor(color);
        layer.get().strokeColor = cgColor;
    }

    void setStrokeWidth(float width) { layer.get().lineWidth = width; }

    void setBounds(const Rect& bounds) { layer.get().bounds = toCGRect(bounds); }

    void setPosition(const Point& pos)
    {
        layer.get().position = CGPointMake(pos.x, pos.y);
    }

    void setHidden(bool hidden) { layer.get().hidden = hidden; }

    void setOpacity(float opacity) { layer.get().opacity = opacity; }

    ObjC::Ptr<CAShapeLayer> layer;
};

ShapeLayer::ShapeLayer()
    : impl()
{
}

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

void* ShapeLayer::getNativeLayer()
{
    return impl.get();
}

AnimationTransaction::AnimationTransaction()
{
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
}

AnimationTransaction::~AnimationTransaction()
{
    [CATransaction commit];
}
} // namespace eacp::Graphics
