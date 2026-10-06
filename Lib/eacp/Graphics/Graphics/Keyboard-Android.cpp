#include "Keyboard-Android.h"

#include "../Window/Window.h"

#include <eacp/Core/Android/Jni.h>
#include <eacp/Core/Utils/Strings.h>

#include <android/input.h>
#include <android/keycodes.h>

// Android has no call that polls the keyboard, so the global queries answer
// from the key events the window has seen while it had focus.

namespace eacp::Graphics
{
namespace
{
struct AndroidKeyMapping
{
    uint16_t keyCode;
    int32_t android;
};

constexpr AndroidKeyMapping androidKeyMappings[] = {
    {KeyCode::A, AKEYCODE_A},
    {KeyCode::S, AKEYCODE_S},
    {KeyCode::D, AKEYCODE_D},
    {KeyCode::F, AKEYCODE_F},
    {KeyCode::H, AKEYCODE_H},
    {KeyCode::G, AKEYCODE_G},
    {KeyCode::Z, AKEYCODE_Z},
    {KeyCode::X, AKEYCODE_X},
    {KeyCode::C, AKEYCODE_C},
    {KeyCode::V, AKEYCODE_V},
    {KeyCode::B, AKEYCODE_B},
    {KeyCode::Q, AKEYCODE_Q},
    {KeyCode::W, AKEYCODE_W},
    {KeyCode::E, AKEYCODE_E},
    {KeyCode::R, AKEYCODE_R},
    {KeyCode::Y, AKEYCODE_Y},
    {KeyCode::T, AKEYCODE_T},
    {KeyCode::O, AKEYCODE_O},
    {KeyCode::U, AKEYCODE_U},
    {KeyCode::I, AKEYCODE_I},
    {KeyCode::P, AKEYCODE_P},
    {KeyCode::L, AKEYCODE_L},
    {KeyCode::J, AKEYCODE_J},
    {KeyCode::K, AKEYCODE_K},
    {KeyCode::N, AKEYCODE_N},
    {KeyCode::M, AKEYCODE_M},

    {KeyCode::Num0, AKEYCODE_0},
    {KeyCode::Num1, AKEYCODE_1},
    {KeyCode::Num2, AKEYCODE_2},
    {KeyCode::Num3, AKEYCODE_3},
    {KeyCode::Num4, AKEYCODE_4},
    {KeyCode::Num5, AKEYCODE_5},
    {KeyCode::Num6, AKEYCODE_6},
    {KeyCode::Num7, AKEYCODE_7},
    {KeyCode::Num8, AKEYCODE_8},
    {KeyCode::Num9, AKEYCODE_9},

    {KeyCode::Space, AKEYCODE_SPACE},
    {KeyCode::Return, AKEYCODE_ENTER},
    {KeyCode::Return, AKEYCODE_DPAD_CENTER},
    {KeyCode::Tab, AKEYCODE_TAB},

    // KeyCode::Delete is backspace, which Android calls DEL.
    {KeyCode::Delete, AKEYCODE_DEL},
    {KeyCode::ForwardDelete, AKEYCODE_FORWARD_DEL},

    {KeyCode::Escape, AKEYCODE_ESCAPE},
    {KeyCode::Back, AKEYCODE_BACK},

    {KeyCode::LeftArrow, AKEYCODE_DPAD_LEFT},
    {KeyCode::RightArrow, AKEYCODE_DPAD_RIGHT},
    {KeyCode::DownArrow, AKEYCODE_DPAD_DOWN},
    {KeyCode::UpArrow, AKEYCODE_DPAD_UP},

    {KeyCode::F1, AKEYCODE_F1},
    {KeyCode::F2, AKEYCODE_F2},
    {KeyCode::F3, AKEYCODE_F3},
    {KeyCode::F4, AKEYCODE_F4},
    {KeyCode::F5, AKEYCODE_F5},
    {KeyCode::F6, AKEYCODE_F6},
    {KeyCode::F7, AKEYCODE_F7},
    {KeyCode::F8, AKEYCODE_F8},
    {KeyCode::F9, AKEYCODE_F9},
    {KeyCode::F10, AKEYCODE_F10},
    {KeyCode::F11, AKEYCODE_F11},
    {KeyCode::F12, AKEYCODE_F12},

    {KeyCode::Minus, AKEYCODE_MINUS},
    {KeyCode::Equals, AKEYCODE_EQUALS},
    {KeyCode::LeftBracket, AKEYCODE_LEFT_BRACKET},
    {KeyCode::RightBracket, AKEYCODE_RIGHT_BRACKET},
    {KeyCode::Backslash, AKEYCODE_BACKSLASH},
    {KeyCode::Semicolon, AKEYCODE_SEMICOLON},
    {KeyCode::Quote, AKEYCODE_APOSTROPHE},
    {KeyCode::Comma, AKEYCODE_COMMA},
    {KeyCode::Period, AKEYCODE_PERIOD},
    {KeyCode::Slash, AKEYCODE_SLASH},
    {KeyCode::Grave, AKEYCODE_GRAVE},

    {KeyCode::Home, AKEYCODE_MOVE_HOME},
    {KeyCode::End, AKEYCODE_MOVE_END},
    {KeyCode::PageUp, AKEYCODE_PAGE_UP},
    {KeyCode::PageDown, AKEYCODE_PAGE_DOWN},
    {KeyCode::CapsLock, AKEYCODE_CAPS_LOCK},

    {KeyCode::Shift, AKEYCODE_SHIFT_LEFT},
    {KeyCode::RightShift, AKEYCODE_SHIFT_RIGHT},
    {KeyCode::Control, AKEYCODE_CTRL_LEFT},
    {KeyCode::RightControl, AKEYCODE_CTRL_RIGHT},
    {KeyCode::Option, AKEYCODE_ALT_LEFT},
    {KeyCode::RightOption, AKEYCODE_ALT_RIGHT},
    {KeyCode::Command, AKEYCODE_META_LEFT},
    {KeyCode::RightCommand, AKEYCODE_META_RIGHT},

    {KeyCode::KeypadEnter, AKEYCODE_NUMPAD_ENTER},
    {KeyCode::Keypad0, AKEYCODE_NUMPAD_0},
    {KeyCode::Keypad1, AKEYCODE_NUMPAD_1},
    {KeyCode::Keypad2, AKEYCODE_NUMPAD_2},
    {KeyCode::Keypad3, AKEYCODE_NUMPAD_3},
    {KeyCode::Keypad4, AKEYCODE_NUMPAD_4},
    {KeyCode::Keypad5, AKEYCODE_NUMPAD_5},
    {KeyCode::Keypad6, AKEYCODE_NUMPAD_6},
    {KeyCode::Keypad7, AKEYCODE_NUMPAD_7},
    {KeyCode::Keypad8, AKEYCODE_NUMPAD_8},
    {KeyCode::Keypad9, AKEYCODE_NUMPAD_9},
    {KeyCode::KeypadDecimal, AKEYCODE_NUMPAD_DOT},
    {KeyCode::KeypadPlus, AKEYCODE_NUMPAD_ADD},
    {KeyCode::KeypadMinus, AKEYCODE_NUMPAD_SUBTRACT},
    {KeyCode::KeypadMultiply, AKEYCODE_NUMPAD_MULTIPLY},
    {KeyCode::KeypadDivide, AKEYCODE_NUMPAD_DIVIDE},
    {KeyCode::KeypadEquals, AKEYCODE_NUMPAD_EQUALS},
    {KeyCode::KeypadClear, AKEYCODE_NUM_LOCK},
};

struct AndroidKeyJava
{
    void resolve(Jni::Lookup& lookup)
    {
        keyEvent = lookup.findClass("android/view/KeyEvent");
        init = lookup.method(keyEvent, "<init>", "(II)V");
        getUnicodeChar = lookup.method(keyEvent, "getUnicodeChar", "(I)I");
    }

    jclass keyEvent = nullptr;
    jmethodID init = nullptr;
    jmethodID getUnicodeChar = nullptr;
};

struct AndroidKeyboardState
{
    Vector<Key> pressed;
    ModifierKeys modifiers;
};

AndroidKeyboardState& androidKeyboardState()
{
    static auto state = AndroidKeyboardState {};
    return state;
}

bool isModifierKey(uint16_t keyCode)
{
    switch (keyCode)
    {
        case KeyCode::Shift:
        case KeyCode::RightShift:
        case KeyCode::Control:
        case KeyCode::RightControl:
        case KeyCode::Option:
        case KeyCode::RightOption:
        case KeyCode::Command:
        case KeyCode::RightCommand:
            return true;
        default:
            return false;
    }
}
} // namespace

uint16_t androidKeyCodeFromNative(int32_t androidCode)
{
    for (const auto& mapping: androidKeyMappings)
        if (mapping.android == androidCode)
            return mapping.keyCode;

    return KeyCode::Unknown;
}

int32_t androidNativeFromKeyCode(uint16_t keyCode)
{
    for (const auto& mapping: androidKeyMappings)
        if (mapping.keyCode == keyCode)
            return mapping.android;

    return AKEYCODE_UNKNOWN;
}

ModifierKeys androidModifiersFromMeta(int32_t metaState)
{
    auto modifiers = ModifierKeys {};
    modifiers.shift = (metaState & AMETA_SHIFT_ON) != 0;
    modifiers.control = (metaState & AMETA_CTRL_ON) != 0;
    modifiers.alt = (metaState & AMETA_ALT_ON) != 0;
    modifiers.command = (metaState & AMETA_META_ON) != 0;
    return modifiers;
}

// The NDK has no call for it, so it is a KeyEvent made in Java for the key.
std::string androidKeyText(int32_t androidCode, int32_t metaState)
{
    auto* env = Jni::currentEnv();
    const auto* java =
        env != nullptr ? Jni::resolveOnce<AndroidKeyJava>(env) : nullptr;

    if (java == nullptr || androidCode == AKEYCODE_UNKNOWN)
        return {};

    auto frame = Jni::LocalFrame {env};
    auto* key = env->NewObject(
        java->keyEvent, java->init, AKEY_EVENT_ACTION_DOWN, androidCode);
    auto character = Jni::failed(env)
                         ? 0
                         : env->CallIntMethod(key, java->getUnicodeChar, metaState);

    if (Jni::failed(env) || character <= 0)
        return {};

    return Strings::narrow(std::wstring(1, (wchar_t) character));
}

void androidKeyboardEvent(const KeyEvent& event)
{
    auto& state = androidKeyboardState();
    state.modifiers = event.modifiers;

    if (event.keyCode == KeyCode::Unknown)
        return;

    auto isThisKey = [&event](const Key& key)
    { return key.keyCode == event.keyCode; };

    state.pressed.removeIndexesMatching(isThisKey);

    if (event.type == KeyEventType::Down)
        state.pressed.add(Key {event.keyCode, event.charactersIgnoringModifiers});
}

void androidKeyboardReset()
{
    androidKeyboardState() = {};
}

bool Keyboard::isKeyPressed(const Window&, uint16_t keyCode)
{
    return isKeyPressed(keyCode);
}

bool Keyboard::isShiftPressed(const Window&)
{
    return isShiftPressed();
}

bool Keyboard::isControlPressed(const Window&)
{
    return isControlPressed();
}

bool Keyboard::isAltPressed(const Window&)
{
    return isAltPressed();
}

bool Keyboard::isCommandPressed(const Window&)
{
    return isCommandPressed();
}

ModifierKeys Keyboard::getModifiers(const Window&)
{
    return getModifiers();
}

bool Keyboard::isKeyPressed(uint16_t keyCode)
{
    auto isThisKey = [keyCode](const Key& key) { return key.keyCode == keyCode; };

    return androidKeyboardState().pressed.findIf(isThisKey) != nullptr;
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
    return androidKeyboardState().modifiers;
}

std::string Keyboard::keyCodeToCharacter(uint16_t keyCode)
{
    if (isModifierKey(keyCode))
        return {};

    return androidKeyText(androidNativeFromKeyCode(keyCode), 0);
}

Vector<Key> Keyboard::getPressedKeys()
{
    return androidKeyboardState().pressed;
}

} // namespace eacp::Graphics
