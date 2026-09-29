#include "Keyboard-Web.h"

#include "../Window/Window.h"

#include <algorithm>
#include <utility>

namespace eacp::Graphics
{
namespace
{
struct WebKey
{
    std::string_view code;
    uint16_t keyCode = 0;
    std::string_view character;
};

constexpr WebKey webKeys[] = {
    {"KeyA", KeyCode::A, "a"},
    {"KeyB", KeyCode::B, "b"},
    {"KeyC", KeyCode::C, "c"},
    {"KeyD", KeyCode::D, "d"},
    {"KeyE", KeyCode::E, "e"},
    {"KeyF", KeyCode::F, "f"},
    {"KeyG", KeyCode::G, "g"},
    {"KeyH", KeyCode::H, "h"},
    {"KeyI", KeyCode::I, "i"},
    {"KeyJ", KeyCode::J, "j"},
    {"KeyK", KeyCode::K, "k"},
    {"KeyL", KeyCode::L, "l"},
    {"KeyM", KeyCode::M, "m"},
    {"KeyN", KeyCode::N, "n"},
    {"KeyO", KeyCode::O, "o"},
    {"KeyP", KeyCode::P, "p"},
    {"KeyQ", KeyCode::Q, "q"},
    {"KeyR", KeyCode::R, "r"},
    {"KeyS", KeyCode::S, "s"},
    {"KeyT", KeyCode::T, "t"},
    {"KeyU", KeyCode::U, "u"},
    {"KeyV", KeyCode::V, "v"},
    {"KeyW", KeyCode::W, "w"},
    {"KeyX", KeyCode::X, "x"},
    {"KeyY", KeyCode::Y, "y"},
    {"KeyZ", KeyCode::Z, "z"},
    {"Digit0", KeyCode::Num0, "0"},
    {"Digit1", KeyCode::Num1, "1"},
    {"Digit2", KeyCode::Num2, "2"},
    {"Digit3", KeyCode::Num3, "3"},
    {"Digit4", KeyCode::Num4, "4"},
    {"Digit5", KeyCode::Num5, "5"},
    {"Digit6", KeyCode::Num6, "6"},
    {"Digit7", KeyCode::Num7, "7"},
    {"Digit8", KeyCode::Num8, "8"},
    {"Digit9", KeyCode::Num9, "9"},
    {"Space", KeyCode::Space, " "},
    {"Enter", KeyCode::Return, ""},
    {"Tab", KeyCode::Tab, ""},
    {"Backspace", KeyCode::Delete, ""},
    {"Escape", KeyCode::Escape, ""},
    {"ArrowLeft", KeyCode::LeftArrow, ""},
    {"ArrowRight", KeyCode::RightArrow, ""},
    {"ArrowDown", KeyCode::DownArrow, ""},
    {"ArrowUp", KeyCode::UpArrow, ""},
    {"F1", KeyCode::F1, ""},
    {"F2", KeyCode::F2, ""},
    {"F3", KeyCode::F3, ""},
    {"F4", KeyCode::F4, ""},
    {"F5", KeyCode::F5, ""},
    {"F6", KeyCode::F6, ""},
    {"F7", KeyCode::F7, ""},
    {"F8", KeyCode::F8, ""},
    {"F9", KeyCode::F9, ""},
    {"F10", KeyCode::F10, ""},
    {"F11", KeyCode::F11, ""},
    {"F12", KeyCode::F12, ""},
    {"Minus", KeyCode::Minus, "-"},
    {"Equal", KeyCode::Equals, "="},
    {"BracketLeft", KeyCode::LeftBracket, "["},
    {"BracketRight", KeyCode::RightBracket, "]"},
    {"Backslash", KeyCode::Backslash, "\\"},
    {"Semicolon", KeyCode::Semicolon, ";"},
    {"Quote", KeyCode::Quote, "'"},
    {"Comma", KeyCode::Comma, ","},
    {"Period", KeyCode::Period, "."},
    {"Slash", KeyCode::Slash, "/"},
    {"Backquote", KeyCode::Grave, "`"},
    {"Home", KeyCode::Home, ""},
    {"End", KeyCode::End, ""},
    {"PageUp", KeyCode::PageUp, ""},
    {"PageDown", KeyCode::PageDown, ""},
    {"Delete", KeyCode::ForwardDelete, ""},
    {"CapsLock", KeyCode::CapsLock, ""},
    {"NumpadEnter", KeyCode::KeypadEnter, ""},
    {"Numpad0", KeyCode::Keypad0, "0"},
    {"Numpad1", KeyCode::Keypad1, "1"},
    {"Numpad2", KeyCode::Keypad2, "2"},
    {"Numpad3", KeyCode::Keypad3, "3"},
    {"Numpad4", KeyCode::Keypad4, "4"},
    {"Numpad5", KeyCode::Keypad5, "5"},
    {"Numpad6", KeyCode::Keypad6, "6"},
    {"Numpad7", KeyCode::Keypad7, "7"},
    {"Numpad8", KeyCode::Keypad8, "8"},
    {"Numpad9", KeyCode::Keypad9, "9"},
    {"NumpadDecimal", KeyCode::KeypadDecimal, "."},
    {"NumpadAdd", KeyCode::KeypadPlus, "+"},
    {"NumpadSubtract", KeyCode::KeypadMinus, "-"},
    {"NumpadMultiply", KeyCode::KeypadMultiply, "*"},
    {"NumpadDivide", KeyCode::KeypadDivide, "/"},
    {"NumLock", KeyCode::KeypadClear, ""},
    {"NumpadEqual", KeyCode::KeypadEquals, "="},
};

struct WebKeyboardState
{
    Vector<uint16_t> pressed;
    ModifierKeys modifiers;
};

WebKeyboardState& webKeyboard()
{
    static auto state = WebKeyboardState {};
    return state;
}

const WebKey* webFindKey(uint16_t keyCode)
{
    for (const auto& key: webKeys)
        if (key.keyCode == keyCode)
            return &key;

    return nullptr;
}
} // namespace

uint16_t webKeyCodeFromCode(std::string_view code)
{
    for (const auto& key: webKeys)
        if (key.code == code)
            return key.keyCode;

    return KeyCode::Unknown;
}

void webKeyChanged(uint16_t keyCode, bool pressed)
{
    auto& keys = webKeyboard().pressed;

    if (keyCode == KeyCode::Unknown)
        return;

    auto found = std::find(keys.begin(), keys.end(), keyCode);

    if (pressed && found == keys.end())
        keys.add(keyCode);
    else if (!pressed && found != keys.end())
        keys.removeIndexesMatching([keyCode](uint16_t key)
                                   { return key == keyCode; });
}

void webModifiersChanged(const ModifierKeys& modifiers)
{
    webKeyboard().modifiers = modifiers;
}

void webReleaseAllKeys()
{
    webKeyboard() = {};
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
    const auto& keys = webKeyboard().pressed;
    return std::find(keys.begin(), keys.end(), keyCode) != keys.end();
}

bool Keyboard::isShiftPressed()
{
    return webKeyboard().modifiers.shift;
}

bool Keyboard::isControlPressed()
{
    return webKeyboard().modifiers.control;
}

bool Keyboard::isAltPressed()
{
    return webKeyboard().modifiers.alt;
}

bool Keyboard::isCommandPressed()
{
    return webKeyboard().modifiers.command;
}

ModifierKeys Keyboard::getModifiers()
{
    return webKeyboard().modifiers;
}

// The US layout's character: a page cannot ask the layout what a key makes
// until the key is pressed.
std::string Keyboard::keyCodeToCharacter(uint16_t keyCode)
{
    auto* key = webFindKey(keyCode);
    return key != nullptr ? std::string {key->character} : std::string {};
}

Vector<Key> Keyboard::getPressedKeys()
{
    auto result = Vector<Key> {};

    for (auto keyCode: webKeyboard().pressed)
        result.add({keyCode, keyCodeToCharacter(keyCode)});

    return result;
}
} // namespace eacp::Graphics
