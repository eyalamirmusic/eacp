#include <eacp/Core/Utils/WinInclude.h>

#include "Keyboard.h"
#include "../Window/Window.h"
#include "../Helpers/StringUtils-Windows.h"

namespace eacp::Graphics
{

namespace
{
struct KeyMapping
{
    uint16_t keyCode;
    int vk;
};

// The single source of truth between framework KeyCodes (macOS virtual key
// values, see Keyboard.h) and Windows virtual keys; both lookup directions
// derive from it.
constexpr KeyMapping keyMappings[] = {
    {KeyCode::A, 'A'},
    {KeyCode::S, 'S'},
    {KeyCode::D, 'D'},
    {KeyCode::F, 'F'},
    {KeyCode::H, 'H'},
    {KeyCode::G, 'G'},
    {KeyCode::Z, 'Z'},
    {KeyCode::X, 'X'},
    {KeyCode::C, 'C'},
    {KeyCode::V, 'V'},
    {KeyCode::B, 'B'},
    {KeyCode::Q, 'Q'},
    {KeyCode::W, 'W'},
    {KeyCode::E, 'E'},
    {KeyCode::R, 'R'},
    {KeyCode::Y, 'Y'},
    {KeyCode::T, 'T'},
    {KeyCode::O, 'O'},
    {KeyCode::U, 'U'},
    {KeyCode::I, 'I'},
    {KeyCode::P, 'P'},
    {KeyCode::L, 'L'},
    {KeyCode::J, 'J'},
    {KeyCode::K, 'K'},
    {KeyCode::N, 'N'},
    {KeyCode::M, 'M'},

    {KeyCode::Num0, '0'},
    {KeyCode::Num1, '1'},
    {KeyCode::Num2, '2'},
    {KeyCode::Num3, '3'},
    {KeyCode::Num4, '4'},
    {KeyCode::Num5, '5'},
    {KeyCode::Num6, '6'},
    {KeyCode::Num7, '7'},
    {KeyCode::Num8, '8'},
    {KeyCode::Num9, '9'},

    {KeyCode::Space, VK_SPACE},
    {KeyCode::Return, VK_RETURN},
    {KeyCode::Tab, VK_TAB},
    {KeyCode::Delete, VK_BACK},
    {KeyCode::Escape, VK_ESCAPE},

    {KeyCode::LeftArrow, VK_LEFT},
    {KeyCode::RightArrow, VK_RIGHT},
    {KeyCode::DownArrow, VK_DOWN},
    {KeyCode::UpArrow, VK_UP},

    {KeyCode::F1, VK_F1},
    {KeyCode::F2, VK_F2},
    {KeyCode::F3, VK_F3},
    {KeyCode::F4, VK_F4},
    {KeyCode::F5, VK_F5},
    {KeyCode::F6, VK_F6},
    {KeyCode::F7, VK_F7},
    {KeyCode::F8, VK_F8},
    {KeyCode::F9, VK_F9},
    {KeyCode::F10, VK_F10},
    {KeyCode::F11, VK_F11},
    {KeyCode::F12, VK_F12},

    // Punctuation. The OEM_* codes are positional on a US layout, which is the
    // same convention the macOS virtual key codes follow, so a shortcut bound
    // to a physical key means the same thing on both platforms.
    {KeyCode::Minus, VK_OEM_MINUS},
    {KeyCode::Equals, VK_OEM_PLUS},
    {KeyCode::LeftBracket, VK_OEM_4},
    {KeyCode::RightBracket, VK_OEM_6},
    {KeyCode::Backslash, VK_OEM_5},
    {KeyCode::Semicolon, VK_OEM_1},
    {KeyCode::Quote, VK_OEM_7},
    {KeyCode::Comma, VK_OEM_COMMA},
    {KeyCode::Period, VK_OEM_PERIOD},
    {KeyCode::Slash, VK_OEM_2},
    {KeyCode::Grave, VK_OEM_3},

    {KeyCode::Home, VK_HOME},
    {KeyCode::End, VK_END},
    {KeyCode::PageUp, VK_PRIOR},
    {KeyCode::PageDown, VK_NEXT},
    {KeyCode::ForwardDelete, VK_DELETE},
    {KeyCode::CapsLock, VK_CAPITAL},

    // Windows has no separate keypad Enter virtual key -- both Return keys
    // report VK_RETURN and are told apart only by the extended-key flag in the
    // message's lParam, which is not available here. Mapping it to VK_RETURN
    // keeps isKeyPressed(KeypadEnter) meaningful rather than always false.
    {KeyCode::KeypadEnter, VK_RETURN},
    {KeyCode::Keypad0, VK_NUMPAD0},
    {KeyCode::Keypad1, VK_NUMPAD1},
    {KeyCode::Keypad2, VK_NUMPAD2},
    {KeyCode::Keypad3, VK_NUMPAD3},
    {KeyCode::Keypad4, VK_NUMPAD4},
    {KeyCode::Keypad5, VK_NUMPAD5},
    {KeyCode::Keypad6, VK_NUMPAD6},
    {KeyCode::Keypad7, VK_NUMPAD7},
    {KeyCode::Keypad8, VK_NUMPAD8},
    {KeyCode::Keypad9, VK_NUMPAD9},
    {KeyCode::KeypadDecimal, VK_DECIMAL},
    {KeyCode::KeypadPlus, VK_ADD},
    {KeyCode::KeypadMinus, VK_SUBTRACT},
    {KeyCode::KeypadMultiply, VK_MULTIPLY},
    {KeyCode::KeypadDivide, VK_DIVIDE},
    {KeyCode::KeypadClear, VK_CLEAR},
};

int toVirtualKey(uint16_t keyCode)
{
    for (auto& mapping: keyMappings)
        if (mapping.keyCode == keyCode)
            return mapping.vk;

    return 0;
}

std::string characterForVirtualKey(int vk)
{
    unsigned char keyState[256] = {0};
    GetKeyboardState(keyState);

    wchar_t buffer[4] = {0};
    int result = ToUnicode(vk, 0, keyState, buffer, 4, 0);

    if (result > 0)
        return fromWideString(std::wstring(buffer, result));

    return "";
}
} // namespace

// Used by CompositionHostWindow to dispatch KeyEvents in framework KeyCode
// units, so cross-platform comparisons against KeyCode:: constants hold.
uint16_t keyCodeFromVirtualKey(int vk)
{
    for (auto& mapping: keyMappings)
        if (mapping.vk == vk)
            return mapping.keyCode;

    return KeyCode::Unknown;
}

int virtualKeyFromKeyCode(uint16_t keyCode)
{
    return toVirtualKey(keyCode);
}

bool Keyboard::isKeyPressed(uint16_t keyCode)
{
    int vk = toVirtualKey(keyCode);
    if (vk == 0)
        return false;

    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool Keyboard::isShiftPressed()
{
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

bool Keyboard::isControlPressed()
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
}

bool Keyboard::isAltPressed()
{
    return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}

bool Keyboard::isCommandPressed()
{
    // Windows doesn't have Command key, map to Windows key
    return (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0
           || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
}

ModifierKeys Keyboard::getModifiers()
{
    return {
        isShiftPressed(), isControlPressed(), isAltPressed(), isCommandPressed()};
}

Vector<Key> Keyboard::getPressedKeys()
{
    Vector<Key> keys;

    for (auto& mapping: keyMappings)
    {
        if (GetAsyncKeyState(mapping.vk) & 0x8000)
        {
            Key key;
            key.keyCode = mapping.keyCode;
            key.character = characterForVirtualKey(mapping.vk);
            keys.add(key);
        }
    }

    return keys;
}

std::string Keyboard::keyCodeToCharacter(uint16_t keyCode)
{
    int vk = toVirtualKey(keyCode);
    if (vk == 0)
        return "";

    return characterForVirtualKey(vk);
}

// Window-scoped keyboard state implementations
// These delegate to the Window's tracked keyboard state

bool Keyboard::isKeyPressed(const Window& window, uint16_t keyCode)
{
    int vk = toVirtualKey(keyCode);
    if (vk == 0)
        return false;
    return window.isKeyPressed(static_cast<uint16_t>(vk));
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

} // namespace eacp::Graphics
