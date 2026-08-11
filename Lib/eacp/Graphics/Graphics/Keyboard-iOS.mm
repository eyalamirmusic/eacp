#include "Keyboard.h"

namespace eacp::Graphics
{

bool Keyboard::isKeyPressed(uint16_t)
{
    // iOS exposes no hardware keyboard state API.
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

Vector<Key> Keyboard::getPressedKeys()
{
    return {};
}

} // namespace eacp::Graphics
