#include "NativeLayer.h"
#include "ShapeLayer.h"
#include "../Primitives/GraphicUtils.h"

@interface ImmediateShapeLayer : CAShapeLayer
@end

@implementation ImmediateShapeLayer

- (id<CAAction>)actionForKey:(NSString*)event
{
    return [NSNull null];
}

@end

@interface ImmediateGradientLayer : CAGradientLayer
@end

@implementation ImmediateGradientLayer

- (id<CAAction>)actionForKey:(NSString*)event
{
    return [NSNull null];
}

@end

namespace eacp::Graphics
{

struct ShapeLayer::Native : public NativeLayer
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

    void setPath(CGPathRef path)
    {
        layer.get().path = path;
        updateGradientMask();
    }

    void clearPath()
    {
        layer.get().path = nil;
        updateGradientMask();
    }

    void setFillColor(const Color& color)
    {
        removeGradientLayer();
        layer.get().fillColor = toCGColor(color);
    }

    void setFillGradient(const LinearGradient& gradient)
    {
        layer.get().fillColor = nil;

        if (!gradientLayer)
        {
            gradientLayer = [ImmediateGradientLayer layer];
            gradientLayer.get().anchorPoint = CGPointMake(0, 0);
            [layer.get() addSublayer:gradientLayer.get()];
        }

        auto colors = [NSMutableArray array];
        auto locations = [NSMutableArray array];

        for (const auto& stop: gradient.stops)
        {
            [colors addObject:(__bridge id) toCGColor(stop.color).get()];
            [locations addObject:@(stop.position)];
        }

        gradientLayer.get().colors = colors;
        gradientLayer.get().locations = locations;

        currentGradient = gradient;
        updateGradientMask();
    }

    void updateGradientMask()
    {
        if (!gradientLayer)
            return;

        auto cgBounds = CGPathGetBoundingBox(layer.get().path);
        gradientLayer.get().frame = cgBounds;

        auto bounds = toRect(cgBounds);

        if (bounds.w > 0 && bounds.h > 0)
        {
            auto start = bounds.getRelativePoint(currentGradient.start);
            auto end = bounds.getRelativePoint(currentGradient.end);

            gradientLayer.get().startPoint = toCGPoint(start);
            gradientLayer.get().endPoint = toCGPoint(end);
        }

        auto maskLayer = [ImmediateShapeLayer layer];

        auto transform =
            CGAffineTransformMakeTranslation(-cgBounds.origin.x, -cgBounds.origin.y);
        auto translatedPath =
            CGPathCreateCopyByTransformingPath(layer.get().path, &transform);
        maskLayer.path = translatedPath;
        CGPathRelease(translatedPath);

        gradientLayer.get().mask = maskLayer;
    }

    void removeGradientLayer()
    {
        if (gradientLayer)
        {
            [gradientLayer.get() removeFromSuperlayer];
            gradientLayer = nil;
        }
    }

    void setStrokeColor(const Color& color)
    {
        layer.get().strokeColor = toCGColor(color);
    }

    void setStrokeWidth(float width) { layer.get().lineWidth = width; }

    ObjC::Ptr<ImmediateShapeLayer> layer;
    ObjC::Ptr<ImmediateGradientLayer> gradientLayer;
    LinearGradient currentGradient;
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

void ShapeLayer::setFillGradient(const LinearGradient& gradient)
{
    impl->setFillGradient(gradient);
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
