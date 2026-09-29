#include "Keyboard.h"

#include "../Window/Window.h"

namespace eacp::Graphics
{

bool Keyboard::isKeyPressed(const Window&, uint16_t)
{
    return false;
}

bool Keyboard::isShiftPressed(const Window&)
{
    return false;
}

bool Keyboard::isControlPressed(const Window&)
{
    return false;
}

bool Keyboard::isAltPressed(const Window&)
{
    return false;
}

bool Keyboard::isCommandPressed(const Window&)
{
    return false;
}

ModifierKeys Keyboard::getModifiers(const Window&)
{
    return {};
}

bool Keyboard::isKeyPressed(uint16_t)
{
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
