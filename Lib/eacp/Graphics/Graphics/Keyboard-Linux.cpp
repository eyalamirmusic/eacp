#include "Keyboard.h"

// Linux stub, on the same terms as Keyboard-iOS.mm: nothing here invents a
// key state it has no way to know.
//
// Polled keyboard state comes from a compositor, and a Linux Window has no
// surface for one to talk to yet - the Wayland seat, its wl_keyboard and the
// xkbcommon state that answers these arrive with the windowing backend (stage 4
// of plan.md), together with the keysym-to-KeyCode table this file will grow.
// Until then every key reads as up, which is what an app that polls in its
// update loop can carry on against; the event-driven half (View::keyDown) has
// nothing to deliver either.

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

Vector<Key> Keyboard::getPressedKeys()
{
    return {};
}

std::string Keyboard::keyCodeToCharacter(uint16_t)
{
    return "";
}

} // namespace eacp::Graphics
