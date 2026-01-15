#import <QuartzCore/QuartzCore.h>
#import <CoreText/CoreText.h>
#include "TextLayer.h"
#include "NativeLayer.h"
#include <eacp/Core/ObjC/Strings.h>

@interface ImmediateTextLayer : CATextLayer
@end

@implementation ImmediateTextLayer

- (id<CAAction>)actionForKey:(NSString*)event
{
    return [NSNull null];
}

@end

namespace eacp::Graphics
{

struct TextLayer::Native : MacLayer
{
    Native()
    {
        layer = [ImmediateTextLayer layer];
        layer.get().anchorPoint = CGPointMake(0, 0);
        layer.get().wrapped = NO;
        layer.get().truncationMode = kCATruncationEnd;
        layer.get().alignmentMode = kCAAlignmentLeft;

        nativeLayer = layer.get();
    }

    ~Native() override { detach(); }

    void setText(const std::string& text)
    {
        layer.get().string = Strings::toNSString(text);
    }

    void setFont(CTFontRef font)
    {
        layer.get().font = font;
        layer.get().fontSize = CTFontGetSize(font);
    }

    void setColor(const Color& color)
    {
        layer.get().foregroundColor = toCGColor(color);
    }

    void setBounds(const Rect& bounds) { layer.get().bounds = toCGRect(bounds); }

    void setPosition(const Point& pos)
    {
        layer.get().position = CGPointMake(pos.x, pos.y);
    }

    void setHidden(bool hidden) { layer.get().hidden = hidden; }

    void setOpacity(float opacity) { layer.get().opacity = opacity; }

    ObjC::Ptr<ImmediateTextLayer> layer;
};

TextLayer::TextLayer()
    : impl()
{
}

void TextLayer::setText(const std::string& text)
{
    impl->setText(text);
}

void TextLayer::setFont(const Font& font)
{
    impl->setFont((CTFontRef) font.getHandle());
}

void TextLayer::setColor(const Color& color)
{
    impl->setColor(color);
}

void TextLayer::setBounds(const Rect& bounds)
{
    impl->setBounds(bounds);
}

void TextLayer::setPosition(const Point& position)
{
    impl->setPosition(position);
}

void TextLayer::setHidden(bool hidden)
{
    impl->setHidden(hidden);
}

void TextLayer::setOpacity(float opacity)
{
    impl->setOpacity(opacity);
}

void* TextLayer::getNativeLayer()
{
    return impl.get();
}
} // namespace eacp::Graphics
