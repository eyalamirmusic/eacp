#include "Common.h"

#include <eacp/Core/Utils/Logging.h>
#include <eacp/Graphics/Input/EvdevGamepad-Linux.h>

#include <linux/input.h>
#include <linux/uinput.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>

// GameInputTests.cpp covers the queue and the window feed; this covers the
// evdev backend: the mapping from kernel codes onto GamepadButton and
// GamepadAxis for each driver family, with no device, and then the whole path
// through a virtual pad made with uinput, which skips where /dev/uinput or the
// node it makes is not the user's to open.

using namespace nano;
using namespace eacp::Graphics;

namespace GameInputLinuxTests
{
struct AxisSpec
{
    int code;
    EvdevAxisRange range;
};

struct Event
{
    uint16_t type;
    uint16_t code;
    int value;
};

constexpr auto stick16 = EvdevAxisRange {-32768, 32767};
constexpr auto byteRange = EvdevAxisRange {0, 255};
constexpr auto hatRange = EvdevAxisRange {-1, 1};

EvdevDeviceInfo deviceInfo(std::string driver,
                           uint16_t vendor,
                           std::initializer_list<int> keys,
                           std::initializer_list<AxisSpec> axes)
{
    auto info = EvdevDeviceInfo {};
    info.driver = std::move(driver);
    info.vendor = vendor;

    for (auto key: keys)
        info.keys.set((std::size_t) key);

    for (auto& axis: axes)
    {
        info.axes.set((std::size_t) axis.code);
        info.ranges[axis.code] = axis.range;
    }

    return info;
}

// xpad with an Xbox One pad: label-named face buttons, 10-bit triggers.
EvdevDeviceInfo xpadInfo()
{
    return deviceInfo("xpad",
                      evdevVendorMicrosoft,
                      {BTN_A,
                       BTN_B,
                       BTN_X,
                       BTN_Y,
                       BTN_TL,
                       BTN_TR,
                       BTN_SELECT,
                       BTN_START,
                       BTN_MODE,
                       BTN_THUMBL,
                       BTN_THUMBR},
                      {{ABS_X, stick16},
                       {ABS_Y, stick16},
                       {ABS_RX, stick16},
                       {ABS_RY, stick16},
                       {ABS_Z, {0, 1023}},
                       {ABS_RZ, {0, 1023}},
                       {ABS_HAT0X, hatRange},
                       {ABS_HAT0Y, hatRange}});
}

// hid-playstation with a DualSense: positional, byte sticks, analog triggers
// that also send BTN_TL2/BTN_TR2.
EvdevDeviceInfo dualSenseInfo()
{
    return deviceInfo("playstation",
                      evdevVendorSony,
                      {BTN_SOUTH,
                       BTN_EAST,
                       BTN_NORTH,
                       BTN_WEST,
                       BTN_TL,
                       BTN_TR,
                       BTN_TL2,
                       BTN_TR2,
                       BTN_SELECT,
                       BTN_START,
                       BTN_MODE,
                       BTN_THUMBL,
                       BTN_THUMBR},
                      {{ABS_X, byteRange},
                       {ABS_Y, byteRange},
                       {ABS_RX, byteRange},
                       {ABS_RY, byteRange},
                       {ABS_Z, byteRange},
                       {ABS_RZ, byteRange},
                       {ABS_HAT0X, hatRange},
                       {ABS_HAT0Y, hatRange}});
}

// hid-nintendo with a Pro Controller: digital ZL/ZR, d-pad as buttons, Capture
// as BTN_Z.
EvdevDeviceInfo switchProInfo()
{
    return deviceInfo("nintendo",
                      evdevVendorNintendo,
                      {BTN_SOUTH,
                       BTN_EAST,
                       BTN_NORTH,
                       BTN_WEST,
                       BTN_TL,
                       BTN_TR,
                       BTN_TL2,
                       BTN_TR2,
                       BTN_SELECT,
                       BTN_START,
                       BTN_MODE,
                       BTN_Z,
                       BTN_THUMBL,
                       BTN_THUMBR,
                       BTN_DPAD_UP,
                       BTN_DPAD_DOWN,
                       BTN_DPAD_LEFT,
                       BTN_DPAD_RIGHT},
                      {{ABS_X, {-32767, 32767}},
                       {ABS_Y, {-32767, 32767}},
                       {ABS_RX, {-32767, 32767}},
                       {ABS_RY, {-32767, 32767}}});
}

// An Xbox pad over Bluetooth through hid-generic: the right stick on Z/RZ and
// the triggers as brake and gas.
EvdevDeviceInfo xboxBluetoothInfo()
{
    return deviceInfo("hid-generic",
                      evdevVendorMicrosoft,
                      {BTN_A,
                       BTN_B,
                       BTN_X,
                       BTN_Y,
                       BTN_TL,
                       BTN_TR,
                       BTN_SELECT,
                       BTN_START,
                       BTN_MODE,
                       BTN_THUMBL,
                       BTN_THUMBR},
                      {{ABS_X, {0, 65535}},
                       {ABS_Y, {0, 65535}},
                       {ABS_Z, {0, 65535}},
                       {ABS_RZ, {0, 65535}},
                       {ABS_BRAKE, {0, 1023}},
                       {ABS_GAS, {0, 1023}},
                       {ABS_HAT0X, hatRange},
                       {ABS_HAT0Y, hatRange}});
}

// One mapped pad feeding a queue of its own, read back as a frame sees it.
struct PadHarness
{
    explicit PadHarness(const EvdevDeviceInfo& info)
        : pad(info)
    {
        queue.gamepadConnected(0, pad.family(), -1, 1.0);
    }

    // Applies a packet and returns the pad as the next snapshot has it; valid
    // until the next call.
    const GamepadState& send(std::initializer_list<Event> events)
    {
        for (auto& event: events)
            pad.apply(event.type, event.code, event.value);

        return push();
    }

    const GamepadState& push()
    {
        time += 1.0;
        pad.pushTo(queue, 0, time);
        return queue.snapshot(time).gamepads().front();
    }

    GameInputQueue queue;
    EvdevGamepad pad;
    double time = 1.0;
};

bool noButtonDown(const GamepadState& state)
{
    for (auto button = 0; button < GamepadState::buttonCount; ++button)
        if (state.isDown((GamepadButton) button))
            return false;

    return true;
}

auto tDetection = test("GameInput/evdevGamepadsAreThoseReportingBtnGamepad") = []
{
    check(isEvdevGamepad(xpadInfo()));
    check(isEvdevGamepad(dualSenseInfo()));
    check(isEvdevGamepad(switchProInfo()));

    auto keyboard = deviceInfo("atkbd", 0, {KEY_A, KEY_SPACE}, {});
    check(!isEvdevGamepad(keyboard));

    auto sensors =
        deviceInfo("playstation",
                   evdevVendorSony,
                   {},
                   {{ABS_X, stick16}, {ABS_Y, stick16}, {ABS_Z, stick16}});
    sensors.properties.set(INPUT_PROP_ACCELEROMETER);
    check(!isEvdevGamepad(sensors));

    auto accelerometerWithButtons = dualSenseInfo();
    accelerometerWithButtons.properties.set(INPUT_PROP_ACCELEROMETER);
    check(!isEvdevGamepad(accelerometerWithButtons));
};

auto tFamily = test("GameInput/evdevFamilyComesFromTheDriverThenTheVendor") = []
{
    const auto familyOf = [](std::string driver, uint16_t vendor)
    { return evdevGamepadFamily(deviceInfo(std::move(driver), vendor, {}, {})); };

    // A third-party pad (PowerA) on xpad wears Xbox labels.
    check(familyOf("xpad", 0x24c6) == GamepadFamily::Xbox);
    check(familyOf("xpadneo", evdevVendorMicrosoft) == GamepadFamily::Xbox);
    check(familyOf("hid-generic", evdevVendorMicrosoft) == GamepadFamily::Xbox);
    check(familyOf("playstation", evdevVendorSony) == GamepadFamily::PlayStation);
    check(familyOf("sony", 0x0f0d) == GamepadFamily::PlayStation);
    check(familyOf("", evdevVendorSony) == GamepadFamily::PlayStation);
    check(familyOf("nintendo", evdevVendorNintendo) == GamepadFamily::Nintendo);
    check(familyOf("hid-generic", 0x1234) == GamepadFamily::Generic);
    check(familyOf("", 0) == GamepadFamily::Generic);

    check(evdevFaceButtonsByLabel(xpadInfo()));
    check(evdevFaceButtonsByLabel(xboxBluetoothInfo()));
    check(!evdevFaceButtonsByLabel(dualSenseInfo()));
    check(!evdevFaceButtonsByLabel(switchProInfo()));
};

auto tNormalise = test("GameInput/evdevAxesNormaliseOverTheirRange") = []
{
    check(same(evdevStickValue(-32768, stick16), -1.0));
    check(same(evdevStickValue(32767, stick16), 1.0));
    check(std::abs(evdevStickValue(0, stick16)) < 0.001f);
    check(same(evdevStickValue(0, byteRange), -1.0));
    check(same(evdevStickValue(255, byteRange), 1.0));
    check(std::abs(evdevStickValue(128, byteRange)) < 0.01f);
    check(same(evdevStickValue(400, byteRange), 1.0));
    check(same(evdevStickValue(-5, byteRange), -1.0));
    check(evdevStickValue(7, {0, 0}) == 0.0f);

    check(evdevTriggerValue(0, {0, 1023}) == 0.0f);
    check(same(evdevTriggerValue(1023, {0, 1023}), 1.0));
    check(std::abs(evdevTriggerValue(512, {0, 1023}) - 0.5f) < 0.001f);
    check(evdevTriggerValue(-10, byteRange) == 0.0f);
    check(same(evdevTriggerValue(300, byteRange), 1.0));
    check(evdevTriggerValue(7, {5, 5}) == 0.0f);
};

auto tXpadFaces = test("GameInput/xpadFaceButtonsAreNamedByPosition") = []
{
    auto harness = PadHarness {xpadInfo()};
    check(harness.pad.family() == GamepadFamily::Xbox);

    // xpad sends BTN_X for the left button and BTN_Y for the top one.
    const auto& left = harness.send({{EV_KEY, BTN_X, 1}});
    check(left.isDown(GamepadButton::West));
    check(!left.isDown(GamepadButton::North));

    const auto& top = harness.send({{EV_KEY, BTN_X, 0}, {EV_KEY, BTN_Y, 1}});
    check(top.isDown(GamepadButton::North));
    check(top.wasReleased(GamepadButton::West));

    const auto& rest = harness.send({{EV_KEY, BTN_Y, 0},
                                     {EV_KEY, BTN_A, 1},
                                     {EV_KEY, BTN_B, 1},
                                     {EV_KEY, BTN_SELECT, 1},
                                     {EV_KEY, BTN_START, 1},
                                     {EV_KEY, BTN_MODE, 1},
                                     {EV_KEY, BTN_TL, 1},
                                     {EV_KEY, BTN_THUMBR, 1}});
    check(rest.isDown(GamepadButton::South));
    check(rest.isDown(GamepadButton::East));
    check(rest.isDown(GamepadButton::Back));
    check(rest.isDown(GamepadButton::Start));
    check(rest.isDown(GamepadButton::Home));
    check(rest.isDown(GamepadButton::LeftShoulder));
    check(rest.isDown(GamepadButton::RightStick));
    check(!rest.isDown(GamepadButton::North));
};

auto tPositionalFaces =
    test("GameInput/positionalDriversKeepTheKernelsFaceButtons") = []
{
    for (auto info: {dualSenseInfo(), switchProInfo()})
    {
        auto harness = PadHarness {info};

        const auto& top = harness.send({{EV_KEY, BTN_NORTH, 1}});
        check(top.isDown(GamepadButton::North));
        check(!top.isDown(GamepadButton::West));

        const auto& left =
            harness.send({{EV_KEY, BTN_NORTH, 0}, {EV_KEY, BTN_WEST, 1}});
        check(left.isDown(GamepadButton::West));
        check(!left.isDown(GamepadButton::North));

        const auto& bottom =
            harness.send({{EV_KEY, BTN_WEST, 0}, {EV_KEY, BTN_SOUTH, 1}});
        check(bottom.isDown(GamepadButton::South));
    }
};

auto tSticks = test("GameInput/evdevSticksPointUp") = []
{
    auto harness = PadHarness {xpadInfo()};

    const auto& pushed = harness.send({{EV_ABS, ABS_X, 32767},
                                       {EV_ABS, ABS_Y, -32768},
                                       {EV_ABS, ABS_RX, -32768},
                                       {EV_ABS, ABS_RY, 32767}});
    check(same(pushed.leftStick(), {1.0f, 1.0f}));
    check(same(pushed.rightStick(), {-1.0f, -1.0f}));

    auto ps = PadHarness {dualSenseInfo()};
    const auto& top = ps.send({{EV_ABS, ABS_Y, 0}, {EV_ABS, ABS_X, 255}});
    check(same(top.axis(GamepadAxis::LeftY), 1.0));
    check(same(top.axis(GamepadAxis::LeftX), 1.0));
};

auto tAnalogTriggers = test("GameInput/analogTriggersWinOverTheirButtons") = []
{
    auto harness = PadHarness {dualSenseInfo()};

    const auto& button = harness.send({{EV_KEY, BTN_TL2, 1}, {EV_ABS, ABS_Z, 0}});
    check(button.axis(GamepadAxis::LeftTrigger) == 0.0f);
    check(noButtonDown(button));

    const auto& pulled = harness.send({{EV_ABS, ABS_Z, 255}, {EV_ABS, ABS_RZ, 128}});
    check(same(pulled.axis(GamepadAxis::LeftTrigger), 1.0));
    check(std::abs(pulled.axis(GamepadAxis::RightTrigger) - 0.5f) < 0.01f);

    auto xbox = PadHarness {xpadInfo()};
    const auto& full = xbox.send({{EV_ABS, ABS_RZ, 1023}});
    check(same(full.axis(GamepadAxis::RightTrigger), 1.0));
    check(full.axis(GamepadAxis::RightX) == 0.0f);
};

auto tDigitalTriggers = test("GameInput/switchZLAndZRAreDigitalTriggers") = []
{
    auto harness = PadHarness {switchProInfo()};
    check(harness.pad.family() == GamepadFamily::Nintendo);

    const auto& pulled = harness.send({{EV_KEY, BTN_TL2, 1}, {EV_KEY, BTN_TR2, 1}});
    check(pulled.axis(GamepadAxis::LeftTrigger) == 1.0f);
    check(pulled.axis(GamepadAxis::RightTrigger) == 1.0f);
    check(noButtonDown(pulled));

    const auto& released = harness.send({{EV_KEY, BTN_TL2, 0}});
    check(released.axis(GamepadAxis::LeftTrigger) == 0.0f);
    check(released.axis(GamepadAxis::RightTrigger) == 1.0f);

    // Capture has no GamepadButton.
    const auto& capture = harness.send({{EV_KEY, BTN_TR2, 0}, {EV_KEY, BTN_Z, 1}});
    check(noButtonDown(capture));
};

auto tBluetoothXbox = test("GameInput/bluetoothXboxRightStickIsOnZ") = []
{
    auto harness = PadHarness {xboxBluetoothInfo()};
    check(harness.pad.family() == GamepadFamily::Xbox);

    const auto& moved = harness.send({{EV_ABS, ABS_Z, 65535},
                                      {EV_ABS, ABS_RZ, 0},
                                      {EV_ABS, ABS_BRAKE, 1023},
                                      {EV_ABS, ABS_GAS, 0},
                                      {EV_KEY, BTN_X, 1}});
    check(same(moved.rightStick(), {1.0f, 1.0f}));
    check(same(moved.axis(GamepadAxis::LeftTrigger), 1.0));
    check(moved.axis(GamepadAxis::RightTrigger) == 0.0f);
    check(moved.isDown(GamepadButton::West));
};

auto tDpad = test("GameInput/evdevDpadComesFromTheHatAndTheButtons") = []
{
    auto harness = PadHarness {xpadInfo()};

    const auto& leftDown =
        harness.send({{EV_ABS, ABS_HAT0X, -1}, {EV_ABS, ABS_HAT0Y, 1}});
    check(leftDown.isDown(GamepadButton::DpadLeft));
    check(leftDown.isDown(GamepadButton::DpadDown));
    check(!leftDown.isDown(GamepadButton::DpadUp));
    check(!leftDown.isDown(GamepadButton::DpadRight));

    const auto& rightUp =
        harness.send({{EV_ABS, ABS_HAT0X, 1}, {EV_ABS, ABS_HAT0Y, -1}});
    check(rightUp.isDown(GamepadButton::DpadRight));
    check(rightUp.isDown(GamepadButton::DpadUp));
    check(rightUp.wasReleased(GamepadButton::DpadLeft));

    const auto& centred =
        harness.send({{EV_ABS, ABS_HAT0X, 0}, {EV_ABS, ABS_HAT0Y, 0}});
    check(noButtonDown(centred));

    auto pro = PadHarness {switchProInfo()};
    const auto& up = pro.send({{EV_KEY, BTN_DPAD_UP, 1}});
    check(up.isDown(GamepadButton::DpadUp));
    check(up.wasPressed(GamepadButton::DpadUp));
};

auto tReset = test("GameInput/evdevResetReleasesEverything") = []
{
    auto harness = PadHarness {dualSenseInfo()};

    const auto& held = harness.send(
        {{EV_KEY, BTN_SOUTH, 1}, {EV_ABS, ABS_X, 255}, {EV_ABS, ABS_Z, 255}});
    check(held.isDown(GamepadButton::South));

    harness.pad.reset();
    const auto& reset = harness.push();
    check(reset.wasReleased(GamepadButton::South));
    check(noButtonDown(reset));

    for (auto axis = 0; axis < GamepadState::axisCount; ++axis)
        check(reset.axis((GamepadAxis) axis) == 0.0f);
};

// A gamepad made through /dev/uinput, as the kernel's gamepad protocol has it,
// with a Sony id so the backend reads it as a PlayStation pad. Invalid where
// /dev/uinput is not the user's to write.
class VirtualGamepad
{
public:
    VirtualGamepad()
        : fd(::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC))
    {
        if (fd >= 0 && !create())
            destroy();
    }

    VirtualGamepad(const VirtualGamepad&) = delete;
    VirtualGamepad& operator=(const VirtualGamepad&) = delete;

    ~VirtualGamepad() { destroy(); }

    bool isValid() const { return fd >= 0; }

    void emit(uint16_t type, uint16_t code, int value) const
    {
        auto event = input_event {};
        event.type = type;
        event.code = code;
        event.value = value;
        const auto written = ::write(fd, &event, sizeof(event));
        (void) written;
    }

    void sync() const { emit(EV_SYN, SYN_REPORT, 0); }

    // The eventN node the kernel made for it, or empty.
    std::string node() const
    {
        char sysname[64] {};

        if (::ioctl(fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0)
            return {};

        auto error = std::error_code {};
        const auto directory =
            std::filesystem::path {"/sys/devices/virtual/input"} / sysname;

        for (auto& entry: std::filesystem::directory_iterator {directory, error})
        {
            auto name = entry.path().filename().string();

            if (name.starts_with("event"))
                return name;
        }

        return {};
    }

    void destroy()
    {
        if (fd < 0)
            return;

        ::ioctl(fd, UI_DEV_DESTROY);
        ::close(fd);
        fd = -1;
    }

private:
    bool create() const
    {
        const int keys[] = {BTN_SOUTH,
                            BTN_EAST,
                            BTN_NORTH,
                            BTN_WEST,
                            BTN_TL,
                            BTN_TR,
                            BTN_SELECT,
                            BTN_START,
                            BTN_MODE,
                            BTN_THUMBL,
                            BTN_THUMBR};

        const AxisSpec axes[] = {{ABS_X, stick16},
                                 {ABS_Y, stick16},
                                 {ABS_RX, stick16},
                                 {ABS_RY, stick16},
                                 {ABS_Z, byteRange},
                                 {ABS_RZ, byteRange},
                                 {ABS_HAT0X, hatRange},
                                 {ABS_HAT0Y, hatRange}};

        if (::ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0
            || ::ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0)
            return false;

        for (auto key: keys)
            if (::ioctl(fd, UI_SET_KEYBIT, key) < 0)
                return false;

        for (auto& axis: axes)
        {
            auto setup = uinput_abs_setup {};
            setup.code = (uint16_t) axis.code;
            setup.absinfo.minimum = axis.range.minimum;
            setup.absinfo.maximum = axis.range.maximum;

            if (::ioctl(fd, UI_SET_ABSBIT, axis.code) < 0
                || ::ioctl(fd, UI_ABS_SETUP, &setup) < 0)
                return false;
        }

        auto setup = uinput_setup {};
        setup.id.bustype = BUS_USB;
        setup.id.vendor = evdevVendorSony;
        setup.id.product = 0x0ce6;
        std::strncpy(setup.name, "eacp virtual gamepad", UINPUT_MAX_NAME_SIZE - 1);

        return ::ioctl(fd, UI_DEV_SETUP, &setup) >= 0
               && ::ioctl(fd, UI_DEV_CREATE) >= 0;
    }

    int fd = -1;
};

const GamepadState* findPad(const GameInputFrame& frame, int id)
{
    for (auto& pad: frame.gamepads())
        if (pad.id() == id)
            return &pad;

    return nullptr;
}

// Snapshots until `done` says so or `seconds` pass.
template <typename Done>
bool waitFor(GameInput& input, double seconds, Done&& done)
{
    const auto deadline = GameInput::now() + seconds;

    while (!done(input.snapshot()))
    {
        if (GameInput::now() > deadline)
            return false;

        eacp::Threads::runEventLoopFor(eacp::Time::MS {5});
    }

    return true;
}

std::set<int> padIds(GameInput& input)
{
    auto ids = std::set<int> {};
    waitFor(input, 0.05, [](const GameInputFrame&) { return false; });

    for (auto& pad: input.snapshot().gamepads())
        ids.insert(pad.id());

    return ids;
}

// The first pad in the frame that is not one of `known`, or -1.
int newPadId(const GameInputFrame& frame, const std::set<int>& known)
{
    for (auto& pad: frame.gamepads())
        if (!known.contains(pad.id()))
            return pad.id();

    return -1;
}

bool sawGamepadEvent(const GameInputFrame& frame,
                     int id,
                     InputEventType type,
                     GamepadButton button)
{
    for (auto& event: frame.events())
        if (event.gamepad == id && event.type == type
            && event.code == (uint16_t) button)
            return true;

    return false;
}

auto tVirtualPad = test("GameInput/aVirtualEvdevGamepadIsReported") = []
{
    auto window = Window {};
    auto input = std::optional<GameInput> {};
    input.emplace(window);
    window.events.input.activationChanged(true);

    const auto known = padIds(*input);
    auto device = std::optional<VirtualGamepad> {};
    device.emplace();

    if (!device->isValid())
    {
        eacp::LOG("GameInput: no virtual gamepad test, /dev/uinput is not writable");
        return;
    }

    // Hotplug: the node appears, and udev grants the seat's user access to it
    // a moment later.
    auto id = -1;
    waitFor(*input,
            3.0,
            [&](const GameInputFrame& frame)
            { return (id = newPadId(frame, known)) >= 0; });

    if (id < 0)
    {
        const auto node = "/dev/input/" + device->node();
        const auto readable = ::access(node.c_str(), R_OK) == 0;

        if (!readable)
            eacp::LOG(
                "GameInput: no virtual gamepad test, ", node, " is not readable");

        check(!readable, "the virtual gamepad's node is readable but never arrived");
        return;
    }

    device->emit(EV_KEY, BTN_SOUTH, 1);
    device->emit(EV_KEY, BTN_NORTH, 1);
    device->emit(EV_ABS, ABS_X, 32767);
    device->emit(EV_ABS, ABS_Y, -32768);
    device->emit(EV_ABS, ABS_RZ, 255);
    device->emit(EV_ABS, ABS_HAT0X, -1);
    device->sync();

    auto sawPress = false;
    const auto pressed =
        waitFor(*input,
                2.0,
                [&](const GameInputFrame& frame)
                {
                    sawPress = sawPress
                               || sawGamepadEvent(frame,
                                                  id,
                                                  InputEventType::GamepadDown,
                                                  GamepadButton::South);
                    const auto* pad = findPad(frame, id);
                    return pad != nullptr && pad->isDown(GamepadButton::South);
                });
    check(pressed);
    check(sawPress);

    const auto* pad = findPad(input->snapshot(), id);
    check(pad != nullptr);

    if (pad != nullptr)
    {
        check(pad->family() == GamepadFamily::PlayStation);
        check(pad->playerIndex() == -1);
        check(pad->isDown(GamepadButton::North));
        check(pad->isDown(GamepadButton::DpadLeft));
        check(same(pad->leftStick(), {1.0f, 1.0f}));
        check(same(pad->axis(GamepadAxis::RightTrigger), 1.0));
    }

    device->emit(EV_KEY, BTN_SOUTH, 0);
    device->sync();

    auto sawRelease = false;
    check(waitFor(*input,
                  2.0,
                  [&](const GameInputFrame& frame)
                  {
                      sawRelease = sawRelease
                                   || sawGamepadEvent(frame,
                                                      id,
                                                      InputEventType::GamepadUp,
                                                      GamepadButton::South);
                      return sawRelease;
                  }));

    // Losing key focus releases the pad; taking it back hands over the whole
    // state again, the stick still held.
    window.events.input.activationChanged(false);
    const auto* released = findPad(input->snapshot(), id);
    check(released != nullptr && released->leftStick().x == 0.0f);

    window.events.input.activationChanged(true);
    check(waitFor(*input,
                  2.0,
                  [&](const GameInputFrame& frame)
                  {
                      const auto* resumed = findPad(frame, id);
                      return resumed != nullptr
                             && same(resumed->leftStick(), {1.0f, 1.0f})
                             && resumed->isDown(GamepadButton::North);
                  }));

    // A GameInput made while the pad is plugged in sees it at once. Every pad
    // gets a new id from the new reader, so it is told apart by count.
    input.reset();
    input.emplace(window);
    window.events.input.activationChanged(true);
    check(input->snapshot().gamepads().size() == known.size() + 1);

    // Unplugging it disconnects it.
    device.reset();
    check(waitFor(*input,
                  2.0,
                  [&](const GameInputFrame& frame)
                  { return frame.gamepads().size() == known.size(); }));
};
} // namespace GameInputLinuxTests
