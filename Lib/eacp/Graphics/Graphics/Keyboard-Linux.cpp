#include "Keyboard-Linux.h"

#include "../Window/WaylandDisplay-Linux.h"
#include "../Window/WaylandInput-Linux.h"
#include "../Window/Window.h"

#include <linux/input-event-codes.h>

// The Linux key table, and the polled state behind Keyboard.h.
//
// Two things are different here from both other backends, and they are the same
// difference twice: Wayland tells a client about the keyboard only while one of
// its surfaces has focus, and there is no way to ask outside that. There is no
// GetAsyncKeyState and no CGEventSourceKeyState - a client that is not focused
// is not told what is being typed, on purpose.
//
// So the "global" half of Keyboard is answered from the same place as the
// window-scoped half: the seat's own state, which exists only while some window
// of this application has focus. On a machine with no seat at all - a headless
// compositor advertises none - every query below reads as nothing pressed,
// which is what an app polling in its update loop can carry on against.
//
// The table itself is evdev keycodes, which is what wl_keyboard.key carries and
// what xkb calls a keycode once 8 has been added. Positional, like the macOS
// virtual key codes the framework's KeyCode values are, so a shortcut bound to
// a physical key means the same thing on both.

namespace eacp::Graphics
{
namespace
{
struct WaylandKeyMapping
{
    uint16_t keyCode;
    uint32_t evdev;
};

// The single source of truth between framework KeyCodes and the kernel's key
// numbering; both lookup directions derive from it, and
// Tests/Graphics/KeyCodeTests-Linux.cpp checks that every constant in
// Keyboard.h appears exactly once.
constexpr WaylandKeyMapping waylandKeyMappings[] = {
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

    // KeyCode::Delete is backspace and ForwardDelete is the other one; the
    // names in Keyboard.h follow what the key does rather than what a platform
    // calls it, and evdev's names go the other way.
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

    // Punctuation, named for the unshifted key on a US layout - the same
    // convention the macOS virtual key codes and the Windows OEM_* codes
    // follow, so a positional shortcut survives the crossing.
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

    // Clear is the Apple keypad's top-left key, which on a PC keyboard is the
    // one in the same place: Num Lock. Nothing else on the keypad is unclaimed,
    // and leaving it unmapped would make the constant permanently unreachable.
    {KeyCode::KeypadClear, KEY_NUMLOCK},
};

WaylandInput* waylandSeatInput()
{
    auto* connection = waylandDisplay();

    return connection != nullptr ? connection->getInput() : nullptr;
}

// Only while some window of this application has keyboard focus. Wayland tells
// a client nothing about the keyboard otherwise, and inventing an answer would
// be worse than saying so.
bool waylandKeyboardIsFocused()
{
    auto* input = waylandSeatInput();

    return input != nullptr && input->getKeyboardFocus() != nullptr;
}
} // namespace

uint16_t waylandKeyCodeFromEvdev(uint32_t evdevCode)
{
    for (const auto& mapping: waylandKeyMappings)
        if (mapping.evdev == evdevCode)
            return mapping.keyCode;

    return KeyCode::Unknown;
}

uint32_t waylandEvdevFromKeyCode(uint16_t keyCode)
{
    for (const auto& mapping: waylandKeyMappings)
        if (mapping.keyCode == keyCode)
            return mapping.evdev;

    return 0;
}

bool Keyboard::isKeyPressed(const Window& window, uint16_t keyCode)
{
    auto evdev = waylandEvdevFromKeyCode(keyCode);

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
    auto* input = waylandSeatInput();
    auto evdev = waylandEvdevFromKeyCode(keyCode);

    if (input == nullptr || evdev == 0 || !waylandKeyboardIsFocused())
        return false;

    return input->isKeyPressed(evdev);
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
    auto* input = waylandSeatInput();

    if (input == nullptr || !waylandKeyboardIsFocused())
        return {};

    return input->getModifiers();
}

Vector<Key> Keyboard::getPressedKeys()
{
    auto keys = Vector<Key> {};
    auto* input = waylandSeatInput();

    if (input == nullptr || !waylandKeyboardIsFocused())
        return keys;

    for (auto evdev: input->getPressedCodes())
    {
        auto keyCode = waylandKeyCodeFromEvdev(evdev);

        // A key with no framework name - a media key, a layout's extra - is not
        // reportable through Key, whose whole content is a KeyCode.
        if (keyCode == KeyCode::Unknown)
            continue;

        keys.add(Key {keyCode, input->characterForCode(evdev)});
    }

    return keys;
}

std::string Keyboard::keyCodeToCharacter(uint16_t keyCode)
{
    auto* input = waylandSeatInput();
    auto evdev = waylandEvdevFromKeyCode(keyCode);

    if (input == nullptr || evdev == 0)
        return "";

    // Not gated on focus, unlike the state queries above: this asks what the
    // layout would type, not what is being typed, and the keymap outlives the
    // focus that delivered it.
    return input->characterForCode(evdev);
}

} // namespace eacp::Graphics
