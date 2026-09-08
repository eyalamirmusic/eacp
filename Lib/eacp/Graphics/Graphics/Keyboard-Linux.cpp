#include "Keyboard-Linux.h"

#include "../Window/LinuxSeat-Linux.h"
#include "../Window/LinuxWindowSystem-Linux.h"
#include "../Window/Window.h"

#include <linux/input-event-codes.h>

// A Linux seat tells a client about the keyboard only while it has focus, so
// the global queries read as nothing pressed while unfocused or with no seat.

namespace eacp::Graphics
{
namespace
{
struct LinuxKeyMapping
{
    uint16_t keyCode;
    uint32_t evdev;
};

constexpr LinuxKeyMapping linuxKeyMappings[] = {
    {KeyCode::A, KEY_A},
    {KeyCode::S, KEY_S},
    {KeyCode::D, KEY_D},
    {KeyCode::F, KEY_F},
    {KeyCode::H, KEY_H},
    {KeyCode::G, KEY_G},
    {KeyCode::Z, KEY_Z},
    {KeyCode::X, KEY_X},
    {KeyCode::C, KEY_C},
    {KeyCode::V, KEY_V},
    {KeyCode::B, KEY_B},
    {KeyCode::Q, KEY_Q},
    {KeyCode::W, KEY_W},
    {KeyCode::E, KEY_E},
    {KeyCode::R, KEY_R},
    {KeyCode::Y, KEY_Y},
    {KeyCode::T, KEY_T},
    {KeyCode::O, KEY_O},
    {KeyCode::U, KEY_U},
    {KeyCode::I, KEY_I},
    {KeyCode::P, KEY_P},
    {KeyCode::L, KEY_L},
    {KeyCode::J, KEY_J},
    {KeyCode::K, KEY_K},
    {KeyCode::N, KEY_N},
    {KeyCode::M, KEY_M},

    {KeyCode::Num0, KEY_0},
    {KeyCode::Num1, KEY_1},
    {KeyCode::Num2, KEY_2},
    {KeyCode::Num3, KEY_3},
    {KeyCode::Num4, KEY_4},
    {KeyCode::Num5, KEY_5},
    {KeyCode::Num6, KEY_6},
    {KeyCode::Num7, KEY_7},
    {KeyCode::Num8, KEY_8},
    {KeyCode::Num9, KEY_9},

    {KeyCode::Space, KEY_SPACE},
    {KeyCode::Return, KEY_ENTER},
    {KeyCode::Tab, KEY_TAB},

    // KeyCode::Delete is backspace; evdev's names go the other way.
    {KeyCode::Delete, KEY_BACKSPACE},
    {KeyCode::ForwardDelete, KEY_DELETE},

    {KeyCode::Escape, KEY_ESC},

    {KeyCode::LeftArrow, KEY_LEFT},
    {KeyCode::RightArrow, KEY_RIGHT},
    {KeyCode::DownArrow, KEY_DOWN},
    {KeyCode::UpArrow, KEY_UP},

    {KeyCode::F1, KEY_F1},
    {KeyCode::F2, KEY_F2},
    {KeyCode::F3, KEY_F3},
    {KeyCode::F4, KEY_F4},
    {KeyCode::F5, KEY_F5},
    {KeyCode::F6, KEY_F6},
    {KeyCode::F7, KEY_F7},
    {KeyCode::F8, KEY_F8},
    {KeyCode::F9, KEY_F9},
    {KeyCode::F10, KEY_F10},
    {KeyCode::F11, KEY_F11},
    {KeyCode::F12, KEY_F12},

    // Punctuation, named for the unshifted key on a US layout.
    {KeyCode::Minus, KEY_MINUS},
    {KeyCode::Equals, KEY_EQUAL},
    {KeyCode::LeftBracket, KEY_LEFTBRACE},
    {KeyCode::RightBracket, KEY_RIGHTBRACE},
    {KeyCode::Backslash, KEY_BACKSLASH},
    {KeyCode::Semicolon, KEY_SEMICOLON},
    {KeyCode::Quote, KEY_APOSTROPHE},
    {KeyCode::Comma, KEY_COMMA},
    {KeyCode::Period, KEY_DOT},
    {KeyCode::Slash, KEY_SLASH},
    {KeyCode::Grave, KEY_GRAVE},

    {KeyCode::Home, KEY_HOME},
    {KeyCode::End, KEY_END},
    {KeyCode::PageUp, KEY_PAGEUP},
    {KeyCode::PageDown, KEY_PAGEDOWN},
    {KeyCode::CapsLock, KEY_CAPSLOCK},

    {KeyCode::KeypadEnter, KEY_KPENTER},
    {KeyCode::Keypad0, KEY_KP0},
    {KeyCode::Keypad1, KEY_KP1},
    {KeyCode::Keypad2, KEY_KP2},
    {KeyCode::Keypad3, KEY_KP3},
    {KeyCode::Keypad4, KEY_KP4},
    {KeyCode::Keypad5, KEY_KP5},
    {KeyCode::Keypad6, KEY_KP6},
    {KeyCode::Keypad7, KEY_KP7},
    {KeyCode::Keypad8, KEY_KP8},
    {KeyCode::Keypad9, KEY_KP9},
    {KeyCode::KeypadDecimal, KEY_KPDOT},
    {KeyCode::KeypadPlus, KEY_KPPLUS},
    {KeyCode::KeypadMinus, KEY_KPMINUS},
    {KeyCode::KeypadMultiply, KEY_KPASTERISK},
    {KeyCode::KeypadDivide, KEY_KPSLASH},
    {KeyCode::KeypadEquals, KEY_KPEQUAL},

    // Clear is the Apple keypad's top-left key; Num Lock is in that place.
    {KeyCode::KeypadClear, KEY_NUMLOCK},
};

LinuxSeat* linuxFocusedSeat()
{
    auto* seat = linuxSeat();

    return (seat != nullptr && seat->hasKeyboardFocus()) ? seat : nullptr;
}
} // namespace

uint16_t linuxKeyCodeFromEvdev(uint32_t evdevCode)
{
    for (const auto& mapping: linuxKeyMappings)
        if (mapping.evdev == evdevCode)
            return mapping.keyCode;

    return KeyCode::Unknown;
}

uint32_t linuxEvdevFromKeyCode(uint16_t keyCode)
{
    for (const auto& mapping: linuxKeyMappings)
        if (mapping.keyCode == keyCode)
            return mapping.evdev;

    return 0;
}

bool Keyboard::isKeyPressed(const Window& window, uint16_t keyCode)
{
    auto evdev = linuxEvdevFromKeyCode(keyCode);

    if (evdev == 0)
        return false;

    return window.isKeyPressed((uint16_t) evdev);
}

bool Keyboard::isShiftPressed(const Window& window)
{
    return window.isShiftPressed();
}

bool Keyboard::isControlPressed(const Window& window)
{
    return window.isControlPressed();
}

bool Keyboard::isAltPressed(const Window& window)
{
    return window.isAltPressed();
}

bool Keyboard::isCommandPressed(const Window& window)
{
    return window.isCommandPressed();
}

ModifierKeys Keyboard::getModifiers(const Window& window)
{
    return window.getModifiers();
}

bool Keyboard::isKeyPressed(uint16_t keyCode)
{
    auto* seat = linuxFocusedSeat();
    auto evdev = linuxEvdevFromKeyCode(keyCode);

    if (seat == nullptr || evdev == 0)
        return false;

    return seat->isKeyPressed(evdev);
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
    auto* seat = linuxFocusedSeat();

    if (seat == nullptr)
        return {};

    return seat->getModifiers();
}

Vector<Key> Keyboard::getPressedKeys()
{
    auto keys = Vector<Key> {};
    auto* seat = linuxFocusedSeat();

    if (seat == nullptr)
        return keys;

    for (auto evdev: seat->getPressedCodes())
    {
        auto keyCode = linuxKeyCodeFromEvdev(evdev);

        if (keyCode == KeyCode::Unknown)
            continue;

        keys.add(Key {keyCode, seat->characterForCode(evdev)});
    }

    return keys;
}

std::string Keyboard::keyCodeToCharacter(uint16_t keyCode)
{
    auto* seat = linuxSeat();
    auto evdev = linuxEvdevFromKeyCode(keyCode);

    if (seat == nullptr || evdev == 0)
        return "";

    // Not gated on focus: the keymap outlives the focus that delivered it.
    return seat->characterForCode(evdev);
}

} // namespace eacp::Graphics
