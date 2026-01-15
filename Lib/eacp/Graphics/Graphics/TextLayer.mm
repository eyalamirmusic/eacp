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

void* TextLayer::getNativeLayer()
{
    return impl.get();
}
} // namespace eacp::Graphics
