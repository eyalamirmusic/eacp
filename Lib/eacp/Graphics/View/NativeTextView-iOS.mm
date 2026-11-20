#import <UIKit/UIKit.h>

#include "NativeTextView.h"
#include "../Graphics/Keyboard-iOS.h"
#include "../Primitives/GraphicUtils.h"

#include <eacp/Core/ObjC/ObjC.h>
#include <eacp/Core/ObjC/Strings.h>

// Hands a hardware key to the owner first, and to the field only when the
// owner does not keep it.
@interface EACPNativeTextField : UITextView
{
@public
    eacp::Graphics::NativeTextView* owner;
}
@end

@implementation EACPNativeTextField

- (BOOL)isKeptByOwner:(UIPress*)press
{
    using namespace eacp::Graphics;

    if (press.key == nil || owner == nullptr)
        return NO;

    return owner->onKeyDown(iosKeyEventFromPress(press, KeyEventType::Down)) ? YES : NO;
}

- (void)pressesBegan:(NSSet<UIPress*>*)presses withEvent:(UIPressesEvent*)event
{
    auto* unkept = [NSMutableSet<UIPress*> set];

    for (UIPress* press in presses)
    {
        if (![self isKeptByOwner:press])
            [unkept addObject:press];
    }

    if (unkept.count > 0)
        [super pressesBegan:unkept withEvent:event];
}

@end

@interface EACPNativeTextViewDelegate : NSObject <UITextViewDelegate>
{
@public
    eacp::Graphics::NativeTextView* owner;
}
@end

@implementation EACPNativeTextViewDelegate

- (BOOL)textView:(UITextView*)textView
    shouldChangeTextInRange:(NSRange)range
            replacementText:(NSString*)text
{
    if (text.length == 0)
        return YES;

    return owner->onIncomingText(eacp::Strings::toStdString(text)) ? NO : YES;
}

- (void)textViewDidChange:(UITextView*)textView
{
    owner->onTextChanged();
}

@end

namespace eacp::Graphics
{
struct NativeTextView::Native
{
    ObjC::Ptr<EACPNativeTextField> textView {[[EACPNativeTextField alloc] initWithFrame:CGRectZero]};
    ObjC::Ptr<EACPNativeTextViewDelegate> delegate {[[EACPNativeTextViewDelegate alloc] init]};
};

NativeTextView::NativeTextView()
{
    auto* textView = native->textView.get();
    native->delegate.get()->owner = this;
    textView->owner = this;
    textView.delegate = native->delegate.get();
    textView.returnKeyType = UIReturnKeySend;
    textView.backgroundColor = UIColor.clearColor;

    [(__bridge UIView*) getHandle() addSubview:textView];
}

NativeTextView::~NativeTextView()
{
    native->textView.get().delegate = nil;
    native->textView.get()->owner = nullptr;
    [native->textView.get() removeFromSuperview];
}

std::string NativeTextView::getText() const
{
    return Strings::toStdString(native->textView.get().text);
}

void NativeTextView::setText(const std::string& text)
{
    native->textView.get().text = Strings::toNSString(text);
}

void NativeTextView::setFont(const std::string& family, float pointSize)
{
    auto* font = [UIFont fontWithName:Strings::toNSString(family) size:pointSize];

    if (font == nil)
        font = [UIFont systemFontOfSize:pointSize];

    native->textView.get().font = font;
}

void NativeTextView::setColours(Color text, Color background)
{
    auto* textColour = [UIColor colorWithCGColor:toCGColor(text).get()];
    native->textView.get().textColor = textColour;
    native->textView.get().tintColor = textColour;
    native->textView.get().backgroundColor =
        [UIColor colorWithCGColor:toCGColor(background).get()];
}

void NativeTextView::startEditing()
{
    [native->textView.get() becomeFirstResponder];
}

void NativeTextView::stopEditing()
{
    [native->textView.get() resignFirstResponder];
}

bool NativeTextView::isEditing() const
{
    return native->textView.get().isFirstResponder;
}

void NativeTextView::resized()
{
    native->textView.get().frame = toCGRect(getLocalBounds());
}

void* NativeTextView::nativeFocusTarget()
{
    return (__bridge void*) native->textView.get();
}
} // namespace eacp::Graphics
