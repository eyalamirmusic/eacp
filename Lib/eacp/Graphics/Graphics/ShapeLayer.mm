#include "NativeLayer.h"
#include "ShapeLayer.h"

@interface ImmediateShapeLayer : CAShapeLayer
@end

@implementation ImmediateShapeLayer

- (id<CAAction>)actionForKey:(NSString*)event
{
    return [NSNull null];
}

@end

namespace eacp::Graphics
{

struct ShapeLayer::Native : public MacLayer
{
    Native()
    {
        layer = [ImmediateShapeLayer layer];
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
        layer.get().fillColor = toCGColor(color);
    }

    void setStrokeColor(const Color& color)
    {
        layer.get().strokeColor = toCGColor(color);
    }

    void setStrokeWidth(float width) { layer.get().lineWidth = width; }

    ObjC::Ptr<ImmediateShapeLayer> layer;
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

void* ShapeLayer::getNativeLayer()
{
    return impl.get();
}

} // namespace eacp::Graphics
