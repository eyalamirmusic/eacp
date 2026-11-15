#pragma once

#include "GameInputQueue.h"

#include <linux/input-event-codes.h>

#include <bitset>
#include <cstdint>
#include <string>

namespace eacp::Graphics
{

struct EvdevAxisRange
{
    int minimum = 0;
    int maximum = 0;
};

// What an evdev node says about itself, read once when it is opened: the codes
// it can report, the range of each absolute axis, its USB/Bluetooth id and the
// kernel driver bound to it. Plain data, so a mapping can be built and tested
// without a device.
struct EvdevDeviceInfo
{
    std::bitset<KEY_CNT> keys;
    std::bitset<ABS_CNT> axes;
    std::bitset<INPUT_PROP_CNT> properties;
    Array<EvdevAxisRange, ABS_CNT> ranges {};
    uint16_t vendor = 0;
    uint16_t product = 0;

    // The bound driver's name from sysfs ("xpad", "playstation", "nintendo",
    // "hid-generic"...), empty where there is none, as for a uinput device.
    std::string driver;
};

constexpr auto evdevVendorMicrosoft = uint16_t {0x045e};
constexpr auto evdevVendorSony = uint16_t {0x054c};
constexpr auto evdevVendorNintendo = uint16_t {0x057e};

// A gamepad under the kernel's gamepad protocol (Documentation/input/gamepad.rst)
// reports BTN_GAMEPAD, which is BTN_SOUTH. The motion sensors a PlayStation or
// Switch pad exposes as a node of their own report no buttons, and are left out
// by their accelerometer property besides.
bool isEvdevGamepad(const EvdevDeviceInfo& info);

// By driver first, so a third-party pad on xpad is an Xbox one, then by vendor.
GamepadFamily evdevGamepadFamily(const EvdevDeviceInfo& info);

// Whether the device names its face buttons by label rather than by position:
// xpad and xpadneo send BTN_X (which is also BTN_NORTH) for the Xbox X button on
// the left, and BTN_Y (BTN_WEST) for Y on top, as does an Xbox pad over
// Bluetooth through hid-generic. Every positional driver - hid-playstation,
// hid-sony, hid-nintendo - sends BTN_NORTH for the top button.
bool evdevFaceButtonsByLabel(const EvdevDeviceInfo& info);

// A stick axis over its range to -1..1, and a trigger to 0..1, raw: no
// deadzone, as on the other platforms. A range with no width reads 0.
float evdevStickValue(int value, EvdevAxisRange range);
float evdevTriggerValue(int value, EvdevAxisRange range);

// One evdev gamepad's mapping onto GamepadButton and GamepadAxis, and its state
// as the events so far leave it. The device's EV_KEY and EV_ABS events go to
// apply() as they are read, and the SYN_REPORT that closes each packet is when
// the reader calls pushTo(); a SYN_DROPPED is the reader's to handle with
// reset() and the device's current state.
//
// Buttons: the kernel's codes are positional (BTN_SOUTH, BTN_EAST, BTN_NORTH,
// BTN_WEST, BTN_TL/TR, BTN_THUMBL/R, BTN_SELECT as Back, BTN_START, BTN_MODE as
// Home, BTN_DPAD_*), but for the label-named drivers above, whose top and left
// codes are swapped. The d-pad is the BTN_DPAD_* keys and ABS_HAT0X/Y together.
//
// Axes: the left stick is ABS_X/ABS_Y. The right one is ABS_RX/ABS_RY, or
// ABS_Z/ABS_RZ on a pad with no RX/RY (an Xbox pad over Bluetooth). The
// triggers are the Z/RZ pair where the right stick did not take it (xpad,
// hid-playstation), else ABS_BRAKE/ABS_GAS, else ABS_HAT2Y/ABS_HAT2X, and
// where there is no analog trigger at all, BTN_TL2/BTN_TR2 read 0 or 1
// (Switch ZL/ZR). Y is flipped to point up.
class EvdevGamepad
{
public:
    explicit EvdevGamepad(const EvdevDeviceInfo& info);

    GamepadFamily family() const { return kind; }

    // One event of any type; only the EV_KEY and EV_ABS codes the mapping
    // reads change anything.
    void apply(uint16_t type, uint16_t code, int value);

    // Releases every button and returns every axis to rest.
    void reset();

    // Every button and axis as it stands, stamped `time`. The queue drops the
    // buttons that did not change.
    void pushTo(GameInputQueue& queue, int id, double time) const;

private:
    enum class AxisRole : uint8_t
    {
        None,
        LeftX,
        LeftY,
        RightX,
        RightY,
        LeftTrigger,
        RightTrigger,
        HatX,
        HatY
    };

    // A GamepadButton for the key, or Count where it is not one.
    GamepadButton buttonFor(uint16_t code) const;

    void applyKey(uint16_t code, bool down);
    void applyAxis(uint16_t code, int value);

    std::bitset<GamepadState::buttonCount> currentButtons() const;

    Array<AxisRole, ABS_CNT> axisRoles {};
    Array<EvdevAxisRange, ABS_CNT> ranges {};
    GamepadFamily kind = GamepadFamily::Generic;
    bool facesByLabel = false;
    bool digitalLeftTrigger = false;
    bool digitalRightTrigger = false;

    std::bitset<GamepadState::buttonCount> keyButtons;
    int hatX = 0;
    int hatY = 0;
    Array<float, GamepadState::axisCount> axes {};
};

} // namespace eacp::Graphics
