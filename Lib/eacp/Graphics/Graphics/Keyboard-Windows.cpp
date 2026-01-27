// Windows implementation of Keyboard
#include "Keyboard.h"
#include "../Helpers/StringUtils-Windows.h"

#define NOMINMAX
#include <Windows.h>

namespace eacp::Graphics
{

// Map macOS key codes to Windows virtual key codes
static int toVirtualKey(uint16_t keyCode)
{
    // This mapping converts macOS key codes to Windows VK codes
    // Note: macOS uses different key codes than Windows
    switch (keyCode)
    {
        case KeyCode::A:
            return 'A';
        case KeyCode::S:
            return 'S';
        case KeyCode::D:
            return 'D';
        case KeyCode::F:
            return 'F';
        case KeyCode::H:
            return 'H';
        case KeyCode::G:
            return 'G';
        case KeyCode::Z:
            return 'Z';
        case KeyCode::X:
            return 'X';
        case KeyCode::C:
            return 'C';
        case KeyCode::V:
            return 'V';
        case KeyCode::B:
            return 'B';
        case KeyCode::Q:
            return 'Q';
        case KeyCode::W:
            return 'W';
        case KeyCode::E:
            return 'E';
        case KeyCode::R:
            return 'R';
        case KeyCode::Y:
            return 'Y';
        case KeyCode::T:
            return 'T';
        case KeyCode::O:
            return 'O';
        case KeyCode::U:
            return 'U';
        case KeyCode::I:
            return 'I';
        case KeyCode::P:
            return 'P';
        case KeyCode::L:
            return 'L';
        case KeyCode::J:
            return 'J';
        case KeyCode::K:
            return 'K';
        case KeyCode::N:
            return 'N';
        case KeyCode::M:
            return 'M';

        case KeyCode::Num0:
            return '0';
        case KeyCode::Num1:
            return '1';
        case KeyCode::Num2:
            return '2';
        case KeyCode::Num3:
            return '3';
        case KeyCode::Num4:
            return '4';
        case KeyCode::Num5:
            return '5';
        case KeyCode::Num6:
            return '6';
        case KeyCode::Num7:
            return '7';
        case KeyCode::Num8:
            return '8';
        case KeyCode::Num9:
            return '9';

        case KeyCode::Space:
            return VK_SPACE;
        case KeyCode::Return:
            return VK_RETURN;
        case KeyCode::Tab:
            return VK_TAB;
        case KeyCode::Delete:
            return VK_BACK;
        case KeyCode::Escape:
            return VK_ESCAPE;

        case KeyCode::LeftArrow:
            return VK_LEFT;
        case KeyCode::RightArrow:
            return VK_RIGHT;
        case KeyCode::DownArrow:
            return VK_DOWN;
        case KeyCode::UpArrow:
            return VK_UP;

        case KeyCode::F1:
            return VK_F1;
        case KeyCode::F2:
            return VK_F2;
        case KeyCode::F3:
            return VK_F3;
        case KeyCode::F4:
            return VK_F4;
        case KeyCode::F5:
            return VK_F5;
        case KeyCode::F6:
            return VK_F6;
        case KeyCode::F7:
            return VK_F7;
        case KeyCode::F8:
            return VK_F8;
        case KeyCode::F9:
            return VK_F9;
        case KeyCode::F10:
            return VK_F10;
        case KeyCode::F11:
            return VK_F11;
        case KeyCode::F12:
            return VK_F12;

        default:
            return 0;
    }
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

std::vector<Key> Keyboard::getPressedKeys()
{
    std::vector<Key> keys;

    // Check all virtual keys
    for (int vk = 0; vk < 256; ++vk)
    {
        if (GetAsyncKeyState(vk) & 0x8000)
        {
            Key key;
            key.keyCode = static_cast<uint16_t>(vk);
            key.character = keyCodeToCharacter(key.keyCode);
            keys.push_back(key);
        }
    }

    return keys;
}

std::string Keyboard::keyCodeToCharacter(uint16_t keyCode)
{
    int vk = toVirtualKey(keyCode);
    if (vk == 0)
        return "";

    // Get keyboard state
    BYTE keyState[256] = {0};
    GetKeyboardState(keyState);

    // Convert to character
    WCHAR buffer[4] = {0};
    int result = ToUnicode(vk, 0, keyState, buffer, 4, 0);

    if (result > 0)
    {
        // Convert wide char to UTF-8 using WinRT utilities
        return fromWideString(std::wstring(buffer, result));
    }

    return "";
}

} // namespace eacp::Graphics
