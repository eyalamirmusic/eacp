#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>

#include "Keyboard-MacOS.h"

namespace eacp::Graphics
{

ModifierKeys modifierKeysFromEvent(NSEvent* event)
{
    auto flags = event.modifierFlags;

    return {.shift = (flags & NSEventModifierFlagShift) != 0,
            .control = (flags & NSEventModifierFlagControl) != 0,
            .alt = (flags & NSEventModifierFlagOption) != 0,
            .command = (flags & NSEventModifierFlagCommand) != 0};
}

KeyEvent keyEventFrom(NSEvent* event, KeyEventType type)
{
    auto e = KeyEvent();

    if (event.characters != nil)
        e.characters = [event.characters UTF8String];

    if (event.charactersIgnoringModifiers != nil)
    {
        e.charactersIgnoringModifiers =
            [event.charactersIgnoringModifiers UTF8String];
    }

    e.keyCode = event.keyCode;
    e.type = type;
    e.modifiers = modifierKeysFromEvent(event);
    e.isRepeat = event.isARepeat;
    e.timestamp = event.timestamp;

    return e;
}

bool Keyboard::isKeyPressed(uint16_t keyCode)
{
    return CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, keyCode);
}

bool Keyboard::isShiftPressed()
{
    return (NSEvent.modifierFlags & NSEventModifierFlagShift) != 0;
}

bool Keyboard::isControlPressed()
{
    return (NSEvent.modifierFlags & NSEventModifierFlagControl) != 0;
}

bool Keyboard::isAltPressed()
{
    return (NSEvent.modifierFlags & NSEventModifierFlagOption) != 0;
}

bool Keyboard::isCommandPressed()
{
    return (NSEvent.modifierFlags & NSEventModifierFlagCommand) != 0;
}

ModifierKeys Keyboard::getModifiers()
{
    auto flags = NSEvent.modifierFlags;
    return {.shift = (flags & NSEventModifierFlagShift) != 0,
            .control = (flags & NSEventModifierFlagControl) != 0,
            .alt = (flags & NSEventModifierFlagOption) != 0,
            .command = (flags & NSEventModifierFlagCommand) != 0};
}

std::string Keyboard::keyCodeToCharacter(uint16_t keyCode)
{
    auto inputSource = TISCopyCurrentKeyboardInputSource();

    if (inputSource == nullptr)
        return "";

    auto layoutData = static_cast<CFDataRef>(
        TISGetInputSourceProperty(inputSource, kTISPropertyUnicodeKeyLayoutData));

    if (layoutData == nullptr)
    {
        CFRelease(inputSource);
        return "";
    }

    auto keyboardLayout =
        reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(layoutData));

    UInt32 deadKeyState = 0;
    UniChar chars[4];
    UniCharCount actualLength = 0;

    OSStatus status = UCKeyTranslate(keyboardLayout,
                                     keyCode,
                                     kUCKeyActionDown,
                                     0,
                                     LMGetKbdType(),
                                     kUCKeyTranslateNoDeadKeysBit,
                                     &deadKeyState,
                                     4,
                                     &actualLength,
                                     chars);

    CFRelease(inputSource);

    if (status != noErr || actualLength == 0)
        return "";

    auto str = [[NSString alloc] initWithCharacters:chars length:actualLength];
    return [str UTF8String];
}

std::vector<Key> Keyboard::getPressedKeys()
{
    auto pressed = std::vector<Key>();

    for (uint16_t keyCode = 0; keyCode <= 0x7F; ++keyCode)
    {
        if (CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, keyCode))
            pressed.push_back({keyCode, keyCodeToCharacter(keyCode)});
    }

    return pressed;
}

} // namespace eacp::Graphics
