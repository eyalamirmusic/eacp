#include "Keyboard-Web.h"

#include <unordered_map>

namespace eacp::Graphics
{
namespace
{
struct WebKeyMapping
{
    uint16_t keyCode;
    std::string_view code;
};

// KeyboardEvent.code names the key's position on a US layout, whatever the
// layout types, which is what KeyCode names too.
constexpr WebKeyMapping webKeyMappings[] = {
    {KeyCode::A, "KeyA"},
    {KeyCode::S, "KeyS"},
    {KeyCode::D, "KeyD"},
    {KeyCode::F, "KeyF"},
    {KeyCode::H, "KeyH"},
    {KeyCode::G, "KeyG"},
    {KeyCode::Z, "KeyZ"},
    {KeyCode::X, "KeyX"},
    {KeyCode::C, "KeyC"},
    {KeyCode::V, "KeyV"},
    {KeyCode::B, "KeyB"},
    {KeyCode::Q, "KeyQ"},
    {KeyCode::W, "KeyW"},
    {KeyCode::E, "KeyE"},
    {KeyCode::R, "KeyR"},
    {KeyCode::Y, "KeyY"},
    {KeyCode::T, "KeyT"},
    {KeyCode::O, "KeyO"},
    {KeyCode::U, "KeyU"},
    {KeyCode::I, "KeyI"},
    {KeyCode::P, "KeyP"},
    {KeyCode::L, "KeyL"},
    {KeyCode::J, "KeyJ"},
    {KeyCode::K, "KeyK"},
    {KeyCode::N, "KeyN"},
    {KeyCode::M, "KeyM"},

    {KeyCode::Num0, "Digit0"},
    {KeyCode::Num1, "Digit1"},
    {KeyCode::Num2, "Digit2"},
    {KeyCode::Num3, "Digit3"},
    {KeyCode::Num4, "Digit4"},
    {KeyCode::Num5, "Digit5"},
    {KeyCode::Num6, "Digit6"},
    {KeyCode::Num7, "Digit7"},
    {KeyCode::Num8, "Digit8"},
    {KeyCode::Num9, "Digit9"},

    {KeyCode::Space, "Space"},
    {KeyCode::Return, "Enter"},
    {KeyCode::Tab, "Tab"},

    // KeyCode::Delete is backspace; the DOM's names go the other way.
    {KeyCode::Delete, "Backspace"},
    {KeyCode::ForwardDelete, "Delete"},

    {KeyCode::Escape, "Escape"},
    {KeyCode::Back, "BrowserBack"},

    {KeyCode::LeftArrow, "ArrowLeft"},
    {KeyCode::RightArrow, "ArrowRight"},
    {KeyCode::DownArrow, "ArrowDown"},
    {KeyCode::UpArrow, "ArrowUp"},

    {KeyCode::F1, "F1"},
    {KeyCode::F2, "F2"},
    {KeyCode::F3, "F3"},
    {KeyCode::F4, "F4"},
    {KeyCode::F5, "F5"},
    {KeyCode::F6, "F6"},
    {KeyCode::F7, "F7"},
    {KeyCode::F8, "F8"},
    {KeyCode::F9, "F9"},
    {KeyCode::F10, "F10"},
    {KeyCode::F11, "F11"},
    {KeyCode::F12, "F12"},

    {KeyCode::Minus, "Minus"},
    {KeyCode::Equals, "Equal"},
    {KeyCode::LeftBracket, "BracketLeft"},
    {KeyCode::RightBracket, "BracketRight"},
    {KeyCode::Backslash, "Backslash"},
    {KeyCode::Semicolon, "Semicolon"},
    {KeyCode::Quote, "Quote"},
    {KeyCode::Comma, "Comma"},
    {KeyCode::Period, "Period"},
    {KeyCode::Slash, "Slash"},
    {KeyCode::Grave, "Backquote"},

    {KeyCode::Home, "Home"},
    {KeyCode::End, "End"},
    {KeyCode::PageUp, "PageUp"},
    {KeyCode::PageDown, "PageDown"},
    {KeyCode::CapsLock, "CapsLock"},

    {KeyCode::Shift, "ShiftLeft"},
    {KeyCode::RightShift, "ShiftRight"},
    {KeyCode::Control, "ControlLeft"},
    {KeyCode::RightControl, "ControlRight"},
    {KeyCode::Option, "AltLeft"},
    {KeyCode::RightOption, "AltRight"},
    {KeyCode::Command, "MetaLeft"},
    {KeyCode::RightCommand, "MetaRight"},

    {KeyCode::KeypadEnter, "NumpadEnter"},
    {KeyCode::Keypad0, "Numpad0"},
    {KeyCode::Keypad1, "Numpad1"},
    {KeyCode::Keypad2, "Numpad2"},
    {KeyCode::Keypad3, "Numpad3"},
    {KeyCode::Keypad4, "Numpad4"},
    {KeyCode::Keypad5, "Numpad5"},
    {KeyCode::Keypad6, "Numpad6"},
    {KeyCode::Keypad7, "Numpad7"},
    {KeyCode::Keypad8, "Numpad8"},
    {KeyCode::Keypad9, "Numpad9"},
    {KeyCode::KeypadDecimal, "NumpadDecimal"},
    {KeyCode::KeypadPlus, "NumpadAdd"},
    {KeyCode::KeypadMinus, "NumpadSubtract"},
    {KeyCode::KeypadMultiply, "NumpadMultiply"},
    {KeyCode::KeypadDivide, "NumpadDivide"},
    {KeyCode::KeypadEquals, "NumpadEqual"},

    // Clear is the Apple keypad's top-left key; Num Lock is in that place.
    {KeyCode::KeypadClear, "NumLock"},
};

struct WebKeyboardState
{
    Vector<Key> pressed;
    ModifierKeys modifiers;

    // What each key typed when last pressed with nothing held, which is the
    // only way a page learns its layout.
    std::unordered_map<uint16_t, std::string> plainText;
};

WebKeyboardState& webKeyboardState()
{
    static auto state = WebKeyboardState {};
    return state;
}

bool isWebModifierKey(uint16_t keyCode)
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

bool isUtf8Continuation(char byte)
{
    return ((unsigned char) byte & 0xC0) == 0x80;
}

bool isOneCharacter(std::string_view text)
{
    if (text.empty())
        return false;

    auto starts = 0;

    for (auto byte: text)
        if (!isUtf8Continuation(byte))
            ++starts;

    return starts == 1;
}

// The control characters the other platforms type for these, so a view
// checking characters rather than keyCode sees the same on every one.
std::string webNamedKeyText(std::string_view key)
{
    if (key == "Enter")
        return "\r";
    if (key == "Tab")
        return "\t";
    if (key == "Backspace")
        return "\b";
    if (key == "Escape")
        return "\x1b";
    if (key == "Delete")
        return "\x7f";

    return {};
}

std::string webKeyText(std::string_view key)
{
    if (isOneCharacter(key))
        return std::string(key);

    return webNamedKeyText(key);
}

std::string lowercaseAscii(std::string text)
{
    for (auto& character: text)
        if (character >= 'A' && character <= 'Z')
            character = (char) (character - 'A' + 'a');

    return text;
}

// The letter or digit a US layout puts on the key, for a key never pressed
// plain yet.
std::string webDefaultText(uint16_t keyCode)
{
    auto code = webCodeFromKeyCode(keyCode);

    if (code.size() == 4 && code.starts_with("Key"))
        return lowercaseAscii(std::string(code.substr(3)));

    if (code.size() == 6 && code.starts_with("Digit"))
        return std::string(code.substr(5));

    if (keyCode == KeyCode::Space)
        return " ";

    return {};
}

bool hasNoModifiers(const ModifierKeys& modifiers)
{
    return !modifiers.shift && !modifiers.control && !modifiers.alt
           && !modifiers.command;
}
} // namespace

uint16_t webKeyCodeFromCode(std::string_view code)
{
    for (const auto& mapping: webKeyMappings)
        if (mapping.code == code)
            return mapping.keyCode;

    return KeyCode::Unknown;
}

std::string_view webCodeFromKeyCode(uint16_t keyCode)
{
    for (const auto& mapping: webKeyMappings)
        if (mapping.keyCode == keyCode)
            return mapping.code;

    return {};
}

ModifierKeys webModifiersFromMask(int mask)
{
    auto modifiers = ModifierKeys {};
    modifiers.shift = (mask & 1) != 0;
    modifiers.control = (mask & 2) != 0;
    modifiers.alt = (mask & 4) != 0;
    modifiers.command = (mask & 8) != 0;
    return modifiers;
}

KeyEvent webKeyEvent(bool down,
                     std::string_view code,
                     std::string_view key,
                     int modifierMask,
                     bool isRepeat,
                     double timestampMs)
{
    auto event = KeyEvent {};
    event.keyCode = webKeyCodeFromCode(code);
    event.type = down ? KeyEventType::Down : KeyEventType::Up;
    event.modifiers = webModifiersFromMask(modifierMask);
    event.isRepeat = down && isRepeat;
    event.timestamp = timestampMs / 1000.0;

    if (isWebModifierKey(event.keyCode))
        return event;

    event.characters = webKeyText(key);

    auto& plain = webKeyboardState().plainText;

    if (hasNoModifiers(event.modifiers) && !event.characters.empty())
        plain[event.keyCode] = event.characters;

    auto remembered = plain.find(event.keyCode);

    if (remembered != plain.end())
        event.charactersIgnoringModifiers = remembered->second;
    else if (event.modifiers.shift)
        event.charactersIgnoringModifiers = lowercaseAscii(event.characters);
    else
        event.charactersIgnoringModifiers = event.characters;

    return event;
}

void webKeyboardEvent(const KeyEvent& event)
{
    auto& state = webKeyboardState();
    state.modifiers = event.modifiers;

    if (event.keyCode == KeyCode::Unknown)
        return;

    auto isThisKey = [&event](const Key& key)
    { return key.keyCode == event.keyCode; };

    state.pressed.removeIndexesMatching(isThisKey);

    if (event.type == KeyEventType::Down)
        state.pressed.add(Key {event.keyCode, event.charactersIgnoringModifiers});
}

void webKeyboardReset()
{
    auto& state = webKeyboardState();
    state.pressed.clear();
    state.modifiers = {};
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

    return webKeyboardState().pressed.findIf(isThisKey) != nullptr;
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
    return webKeyboardState().modifiers;
}

std::string Keyboard::keyCodeToCharacter(uint16_t keyCode)
{
    if (isWebModifierKey(keyCode))
        return {};

    auto& plain = webKeyboardState().plainText;
    auto remembered = plain.find(keyCode);

    if (remembered != plain.end())
        return remembered->second;

    return webDefaultText(keyCode);
}

Vector<Key> Keyboard::getPressedKeys()
{
    return webKeyboardState().pressed;
}

} // namespace eacp::Graphics
