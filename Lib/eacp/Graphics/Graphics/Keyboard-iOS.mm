#include "Keyboard.h"

namespace eacp::Graphics
{

bool Keyboard::isKeyPressed(uint16_t)
{
    // iOS doesn't have a hardware keyboard state API in the same way as macOS
    return false;
}

bool Keyboard::isShiftPressed()
{
    return false;
}

bool Keyboard::isControlPressed()
{
    return false;
}

bool Keyboard::isAltPressed()
{
    return false;
}

bool Keyboard::isCommandPressed()
{
    return false;
}

ModifierKeys Keyboard::getModifiers()
{
    return {};
}

std::string Keyboard::keyCodeToCharacter(uint16_t)
{
    return "";
}

std::vector<Key> Keyboard::getPressedKeys()
{
    return {};
}

} // namespace eacp::Graphics
