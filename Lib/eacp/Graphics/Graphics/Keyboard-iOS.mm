#include "Keyboard-iOS.h"

#import <UIKit/UIKit.h>

#include <eacp/Core/ObjC/Strings.h>

// UIKit has no call that polls the keyboard, so the global queries answer from
// the hardware key presses the views have seen.

namespace eacp::Graphics
{
namespace
{
struct IosKeyMapping
{
    uint16_t keyCode;
    UIKeyboardHIDUsage hidUsage;
};

constexpr IosKeyMapping iosKeyMappings[] = {
    {KeyCode::A, UIKeyboardHIDUsageKeyboardA},
    {KeyCode::S, UIKeyboardHIDUsageKeyboardS},
    {KeyCode::D, UIKeyboardHIDUsageKeyboardD},
    {KeyCode::F, UIKeyboardHIDUsageKeyboardF},
    {KeyCode::H, UIKeyboardHIDUsageKeyboardH},
    {KeyCode::G, UIKeyboardHIDUsageKeyboardG},
    {KeyCode::Z, UIKeyboardHIDUsageKeyboardZ},
    {KeyCode::X, UIKeyboardHIDUsageKeyboardX},
    {KeyCode::C, UIKeyboardHIDUsageKeyboardC},
    {KeyCode::V, UIKeyboardHIDUsageKeyboardV},
    {KeyCode::B, UIKeyboardHIDUsageKeyboardB},
    {KeyCode::Q, UIKeyboardHIDUsageKeyboardQ},
    {KeyCode::W, UIKeyboardHIDUsageKeyboardW},
    {KeyCode::E, UIKeyboardHIDUsageKeyboardE},
    {KeyCode::R, UIKeyboardHIDUsageKeyboardR},
    {KeyCode::Y, UIKeyboardHIDUsageKeyboardY},
    {KeyCode::T, UIKeyboardHIDUsageKeyboardT},
    {KeyCode::O, UIKeyboardHIDUsageKeyboardO},
    {KeyCode::U, UIKeyboardHIDUsageKeyboardU},
    {KeyCode::I, UIKeyboardHIDUsageKeyboardI},
    {KeyCode::P, UIKeyboardHIDUsageKeyboardP},
    {KeyCode::L, UIKeyboardHIDUsageKeyboardL},
    {KeyCode::J, UIKeyboardHIDUsageKeyboardJ},
    {KeyCode::K, UIKeyboardHIDUsageKeyboardK},
    {KeyCode::N, UIKeyboardHIDUsageKeyboardN},
    {KeyCode::M, UIKeyboardHIDUsageKeyboardM},

    {KeyCode::Num0, UIKeyboardHIDUsageKeyboard0},
    {KeyCode::Num1, UIKeyboardHIDUsageKeyboard1},
    {KeyCode::Num2, UIKeyboardHIDUsageKeyboard2},
    {KeyCode::Num3, UIKeyboardHIDUsageKeyboard3},
    {KeyCode::Num4, UIKeyboardHIDUsageKeyboard4},
    {KeyCode::Num5, UIKeyboardHIDUsageKeyboard5},
    {KeyCode::Num6, UIKeyboardHIDUsageKeyboard6},
    {KeyCode::Num7, UIKeyboardHIDUsageKeyboard7},
    {KeyCode::Num8, UIKeyboardHIDUsageKeyboard8},
    {KeyCode::Num9, UIKeyboardHIDUsageKeyboard9},

    {KeyCode::Space, UIKeyboardHIDUsageKeyboardSpacebar},
    {KeyCode::Return, UIKeyboardHIDUsageKeyboardReturnOrEnter},
    {KeyCode::Tab, UIKeyboardHIDUsageKeyboardTab},
    {KeyCode::Delete, UIKeyboardHIDUsageKeyboardDeleteOrBackspace},
    {KeyCode::ForwardDelete, UIKeyboardHIDUsageKeyboardDeleteForward},
    {KeyCode::Escape, UIKeyboardHIDUsageKeyboardEscape},

    {KeyCode::LeftArrow, UIKeyboardHIDUsageKeyboardLeftArrow},
    {KeyCode::RightArrow, UIKeyboardHIDUsageKeyboardRightArrow},
    {KeyCode::DownArrow, UIKeyboardHIDUsageKeyboardDownArrow},
    {KeyCode::UpArrow, UIKeyboardHIDUsageKeyboardUpArrow},

    {KeyCode::F1, UIKeyboardHIDUsageKeyboardF1},
    {KeyCode::F2, UIKeyboardHIDUsageKeyboardF2},
    {KeyCode::F3, UIKeyboardHIDUsageKeyboardF3},
    {KeyCode::F4, UIKeyboardHIDUsageKeyboardF4},
    {KeyCode::F5, UIKeyboardHIDUsageKeyboardF5},
    {KeyCode::F6, UIKeyboardHIDUsageKeyboardF6},
    {KeyCode::F7, UIKeyboardHIDUsageKeyboardF7},
    {KeyCode::F8, UIKeyboardHIDUsageKeyboardF8},
    {KeyCode::F9, UIKeyboardHIDUsageKeyboardF9},
    {KeyCode::F10, UIKeyboardHIDUsageKeyboardF10},
    {KeyCode::F11, UIKeyboardHIDUsageKeyboardF11},
    {KeyCode::F12, UIKeyboardHIDUsageKeyboardF12},

    {KeyCode::Minus, UIKeyboardHIDUsageKeyboardHyphen},
    {KeyCode::Equals, UIKeyboardHIDUsageKeyboardEqualSign},
    {KeyCode::LeftBracket, UIKeyboardHIDUsageKeyboardOpenBracket},
    {KeyCode::RightBracket, UIKeyboardHIDUsageKeyboardCloseBracket},
    {KeyCode::Backslash, UIKeyboardHIDUsageKeyboardBackslash},
    {KeyCode::Semicolon, UIKeyboardHIDUsageKeyboardSemicolon},
    {KeyCode::Quote, UIKeyboardHIDUsageKeyboardQuote},
    {KeyCode::Comma, UIKeyboardHIDUsageKeyboardComma},
    {KeyCode::Period, UIKeyboardHIDUsageKeyboardPeriod},
    {KeyCode::Slash, UIKeyboardHIDUsageKeyboardSlash},
    {KeyCode::Grave, UIKeyboardHIDUsageKeyboardGraveAccentAndTilde},

    {KeyCode::Home, UIKeyboardHIDUsageKeyboardHome},
    {KeyCode::End, UIKeyboardHIDUsageKeyboardEnd},
    {KeyCode::PageUp, UIKeyboardHIDUsageKeyboardPageUp},
    {KeyCode::PageDown, UIKeyboardHIDUsageKeyboardPageDown},
    {KeyCode::CapsLock, UIKeyboardHIDUsageKeyboardCapsLock},

    {KeyCode::Shift, UIKeyboardHIDUsageKeyboardLeftShift},
    {KeyCode::RightShift, UIKeyboardHIDUsageKeyboardRightShift},
    {KeyCode::Control, UIKeyboardHIDUsageKeyboardLeftControl},
    {KeyCode::RightControl, UIKeyboardHIDUsageKeyboardRightControl},
    {KeyCode::Option, UIKeyboardHIDUsageKeyboardLeftAlt},
    {KeyCode::RightOption, UIKeyboardHIDUsageKeyboardRightAlt},
    {KeyCode::Command, UIKeyboardHIDUsageKeyboardLeftGUI},
    {KeyCode::RightCommand, UIKeyboardHIDUsageKeyboardRightGUI},

    {KeyCode::KeypadEnter, UIKeyboardHIDUsageKeypadEnter},
    {KeyCode::Keypad0, UIKeyboardHIDUsageKeypad0},
    {KeyCode::Keypad1, UIKeyboardHIDUsageKeypad1},
    {KeyCode::Keypad2, UIKeyboardHIDUsageKeypad2},
    {KeyCode::Keypad3, UIKeyboardHIDUsageKeypad3},
    {KeyCode::Keypad4, UIKeyboardHIDUsageKeypad4},
    {KeyCode::Keypad5, UIKeyboardHIDUsageKeypad5},
    {KeyCode::Keypad6, UIKeyboardHIDUsageKeypad6},
    {KeyCode::Keypad7, UIKeyboardHIDUsageKeypad7},
    {KeyCode::Keypad8, UIKeyboardHIDUsageKeypad8},
    {KeyCode::Keypad9, UIKeyboardHIDUsageKeypad9},
    {KeyCode::KeypadDecimal, UIKeyboardHIDUsageKeypadPeriod},
    {KeyCode::KeypadPlus, UIKeyboardHIDUsageKeypadPlus},
    {KeyCode::KeypadMinus, UIKeyboardHIDUsageKeypadHyphen},
    {KeyCode::KeypadMultiply, UIKeyboardHIDUsageKeypadAsterisk},
    {KeyCode::KeypadDivide, UIKeyboardHIDUsageKeypadSlash},
    {KeyCode::KeypadEquals, UIKeyboardHIDUsageKeypadEqualSign},
    {KeyCode::KeypadClear, UIKeyboardHIDUsageKeypadNumLock},
};

struct IosKeyboardState
{
    Vector<Key> pressed;
    ModifierKeys modifiers;
};

IosKeyboardState& iosKeyboardState()
{
    static auto state = IosKeyboardState {};
    return state;
}

uint16_t iosKeyCodeFromHidUsage(UIKeyboardHIDUsage hidUsage)
{
    for (const auto& mapping: iosKeyMappings)
    {
        if (mapping.hidUsage == hidUsage)
            return mapping.keyCode;
    }

    return KeyCode::Unknown;
}

ModifierKeys iosModifiersFromFlags(UIKeyModifierFlags modifierFlags)
{
    auto modifiers = ModifierKeys {};
    modifiers.shift = (modifierFlags & UIKeyModifierShift) != 0;
    modifiers.control = (modifierFlags & UIKeyModifierControl) != 0;
    modifiers.alt = (modifierFlags & UIKeyModifierAlternate) != 0;
    modifiers.command = (modifierFlags & UIKeyModifierCommand) != 0;
    return modifiers;
}
} // namespace

KeyEvent iosKeyEventFromPress(UIPress* press, KeyEventType type)
{
    auto* key = press.key;
    auto event = KeyEvent {};
    event.characters = Strings::toStdString(key.characters);
    event.charactersIgnoringModifiers = Strings::toStdString(key.charactersIgnoringModifiers);
    event.keyCode = iosKeyCodeFromHidUsage(key.keyCode);
    event.type = type;
    event.modifiers = iosModifiersFromFlags(key.modifierFlags);
    event.timestamp = press.timestamp;
    return event;
}

void iosKeyboardEvent(const KeyEvent& event)
{
    auto& state = iosKeyboardState();
    state.modifiers = event.modifiers;

    if (event.keyCode == KeyCode::Unknown)
        return;

    auto isThisKey = [&event](const Key& key)
    { return key.keyCode == event.keyCode; };

    state.pressed.removeIndexesMatching(isThisKey);

    if (event.type == KeyEventType::Down)
        state.pressed.add(Key {event.keyCode, event.charactersIgnoringModifiers});
}

bool Keyboard::isKeyPressed(uint16_t keyCode)
{
    auto isThisKey = [keyCode](const Key& key) { return key.keyCode == keyCode; };

    return iosKeyboardState().pressed.findIf(isThisKey) != nullptr;
}

bool Keyboard::isShiftPressed()
{
    return getModifiers().shift;
}

bool Keyboard::isControlPressed()
{
    return getModifiers().control;
}

bool Keyboard::isAltPressed()
{
    return getModifiers().alt;
}

bool Keyboard::isCommandPressed()
{
    return getModifiers().command;
}

ModifierKeys Keyboard::getModifiers()
{
    return iosKeyboardState().modifiers;
}

// UIKit maps a key to text only for a press in flight; there is no lookup by
// key alone.
std::string Keyboard::keyCodeToCharacter(uint16_t)
{
    return "";
}

Vector<Key> Keyboard::getPressedKeys()
{
    return iosKeyboardState().pressed;
}

} // namespace eacp::Graphics
