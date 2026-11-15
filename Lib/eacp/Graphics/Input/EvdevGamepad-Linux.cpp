#include "EvdevGamepad-Linux.h"

#include <algorithm>
#include <initializer_list>

namespace eacp::Graphics
{
namespace
{
bool isXboxDriver(const std::string& driver)
{
    return driver == "xpad" || driver == "xpadneo";
}

// Where the value sits against the middle of the range: -1 left or up, 1 right
// or down, as gamepad.rst has it for ABS_HAT0X/Y.
int hatDirection(int value, EvdevAxisRange range)
{
    const auto twiceFromCentre =
        2LL * value - (long long) range.minimum - (long long) range.maximum;

    if (twiceFromCentre < 0)
        return -1;

    return twiceFromCentre > 0 ? 1 : 0;
}

// Where the value sits along the range, 0 at the minimum and 1 at the maximum,
// or nothing when the range has no width.
bool unitValue(int value, EvdevAxisRange range, double& unit)
{
    const auto width = (double) range.maximum - (double) range.minimum;

    if (width <= 0.0)
        return false;

    unit = ((double) value - (double) range.minimum) / width;
    return true;
}
} // namespace

bool isEvdevGamepad(const EvdevDeviceInfo& info)
{
    return info.keys.test(BTN_GAMEPAD)
           && !info.properties.test(INPUT_PROP_ACCELEROMETER);
}

GamepadFamily evdevGamepadFamily(const EvdevDeviceInfo& info)
{
    if (isXboxDriver(info.driver))
        return GamepadFamily::Xbox;

    if (info.driver == "playstation" || info.driver == "sony")
        return GamepadFamily::PlayStation;

    if (info.driver == "nintendo")
        return GamepadFamily::Nintendo;

    switch (info.vendor)
    {
        case evdevVendorMicrosoft:
            return GamepadFamily::Xbox;
        case evdevVendorSony:
            return GamepadFamily::PlayStation;
        case evdevVendorNintendo:
            return GamepadFamily::Nintendo;
        default:
            return GamepadFamily::Generic;
    }
}

bool evdevFaceButtonsByLabel(const EvdevDeviceInfo& info)
{
    return isXboxDriver(info.driver) || info.vendor == evdevVendorMicrosoft;
}

float evdevStickValue(int value, EvdevAxisRange range)
{
    auto unit = 0.0;

    if (!unitValue(value, range, unit))
        return 0.0f;

    return (float) std::clamp(unit * 2.0 - 1.0, -1.0, 1.0);
}

float evdevTriggerValue(int value, EvdevAxisRange range)
{
    auto unit = 0.0;

    if (!unitValue(value, range, unit))
        return 0.0f;

    return (float) std::clamp(unit, 0.0, 1.0);
}

EvdevGamepad::EvdevGamepad(const EvdevDeviceInfo& info)
    : ranges(info.ranges)
    , kind(evdevGamepadFamily(info))
    , facesByLabel(evdevFaceButtonsByLabel(info))
{
    const auto has = [&](int code) { return code >= 0 && info.axes.test(code); };

    const auto assign = [&](int code, AxisRole role)
    {
        if (has(code))
            axisRoles[code] = role;
    };

    // The first of the candidates the device has.
    const auto pick = [&](std::initializer_list<int> candidates, AxisRole role)
    {
        for (auto code: candidates)
        {
            if (has(code))
            {
                axisRoles[code] = role;
                return true;
            }
        }

        return false;
    };

    assign(ABS_X, AxisRole::LeftX);
    assign(ABS_Y, AxisRole::LeftY);
    assign(ABS_HAT0X, AxisRole::HatX);
    assign(ABS_HAT0Y, AxisRole::HatY);

    const auto zIsRightStick =
        !(has(ABS_RX) && has(ABS_RY)) && has(ABS_Z) && has(ABS_RZ);

    if (zIsRightStick)
    {
        assign(ABS_Z, AxisRole::RightX);
        assign(ABS_RZ, AxisRole::RightY);
    }
    else
    {
        assign(ABS_RX, AxisRole::RightX);
        assign(ABS_RY, AxisRole::RightY);
    }

    const auto leftZ = zIsRightStick ? -1 : ABS_Z;
    const auto rightZ = zIsRightStick ? -1 : ABS_RZ;

    digitalLeftTrigger = !pick({leftZ, ABS_BRAKE, ABS_HAT2Y}, AxisRole::LeftTrigger)
                         && info.keys.test(BTN_TL2);

    digitalRightTrigger = !pick({rightZ, ABS_GAS, ABS_HAT2X}, AxisRole::RightTrigger)
                          && info.keys.test(BTN_TR2);
}

GamepadButton EvdevGamepad::buttonFor(uint16_t code) const
{
    using Button = GamepadButton;

    switch (code)
    {
        case BTN_SOUTH:
            return Button::South;
        case BTN_EAST:
            return Button::East;
        case BTN_NORTH:
            return facesByLabel ? Button::West : Button::North;
        case BTN_WEST:
            return facesByLabel ? Button::North : Button::West;
        case BTN_TL:
            return Button::LeftShoulder;
        case BTN_TR:
            return Button::RightShoulder;
        case BTN_THUMBL:
            return Button::LeftStick;
        case BTN_THUMBR:
            return Button::RightStick;
        case BTN_START:
            return Button::Start;
        case BTN_SELECT:
            return Button::Back;
        case BTN_MODE:
            return Button::Home;
        case BTN_DPAD_UP:
            return Button::DpadUp;
        case BTN_DPAD_DOWN:
            return Button::DpadDown;
        case BTN_DPAD_LEFT:
            return Button::DpadLeft;
        case BTN_DPAD_RIGHT:
            return Button::DpadRight;
        default:
            return Button::Count;
    }
}

void EvdevGamepad::apply(uint16_t type, uint16_t code, int value)
{
    if (type == EV_KEY && code < KEY_CNT)
        applyKey(code, value != 0);
    else if (type == EV_ABS && code < ABS_CNT)
        applyAxis(code, value);
}

void EvdevGamepad::applyKey(uint16_t code, bool down)
{
    const auto pressure = down ? 1.0f : 0.0f;

    if (code == BTN_TL2 && digitalLeftTrigger)
        axes[(int) GamepadAxis::LeftTrigger] = pressure;
    else if (code == BTN_TR2 && digitalRightTrigger)
        axes[(int) GamepadAxis::RightTrigger] = pressure;

    const auto button = buttonFor(code);

    if (button != GamepadButton::Count)
        keyButtons.set((size_t) button, down);
}

void EvdevGamepad::applyAxis(uint16_t code, int value)
{
    const auto range = ranges[code];
    const auto set = [&](GamepadAxis axis, float to) { axes[(int) axis] = to; };

    switch (axisRoles[code])
    {
        case AxisRole::LeftX:
            set(GamepadAxis::LeftX, evdevStickValue(value, range));
            break;
        case AxisRole::LeftY:
            set(GamepadAxis::LeftY, -evdevStickValue(value, range));
            break;
        case AxisRole::RightX:
            set(GamepadAxis::RightX, evdevStickValue(value, range));
            break;
        case AxisRole::RightY:
            set(GamepadAxis::RightY, -evdevStickValue(value, range));
            break;
        case AxisRole::LeftTrigger:
            set(GamepadAxis::LeftTrigger, evdevTriggerValue(value, range));
            break;
        case AxisRole::RightTrigger:
            set(GamepadAxis::RightTrigger, evdevTriggerValue(value, range));
            break;
        case AxisRole::HatX:
            hatX = hatDirection(value, range);
            break;
        case AxisRole::HatY:
            hatY = hatDirection(value, range);
            break;
        case AxisRole::None:
            break;
    }
}

void EvdevGamepad::reset()
{
    keyButtons.reset();
    hatX = 0;
    hatY = 0;
    axes.fill(0.0f);
}

std::bitset<GamepadState::buttonCount> EvdevGamepad::currentButtons() const
{
    auto buttons = keyButtons;
    const auto setIf = [&](GamepadButton button, bool down)
    {
        if (down)
            buttons.set((size_t) button);
    };

    setIf(GamepadButton::DpadUp, hatY < 0);
    setIf(GamepadButton::DpadDown, hatY > 0);
    setIf(GamepadButton::DpadLeft, hatX < 0);
    setIf(GamepadButton::DpadRight, hatX > 0);
    return buttons;
}

void EvdevGamepad::pushTo(GameInputQueue& queue, int id, double time) const
{
    const auto buttons = currentButtons();

    for (auto button = 0; button < GamepadState::buttonCount; ++button)
        queue.gamepadButtonChanged(
            id, (GamepadButton) button, buttons.test((size_t) button), time);

    for (auto axis = 0; axis < GamepadState::axisCount; ++axis)
        queue.gamepadAxisChanged(id, (GamepadAxis) axis, axes[axis]);
}

} // namespace eacp::Graphics
