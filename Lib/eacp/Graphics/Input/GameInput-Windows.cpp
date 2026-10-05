#include "GameInputBackend.h"
#include <eacp/Core/Utils/Logging.h>
#include <eacp/Core/Utils/WinInclude.h>

#include <Xinput.h>

#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace eacp::Graphics
{
namespace
{
// XInput is poll-only, so one thread of the process polls it for every
// GameInput. A connected slot is read every tick; an empty one is tried again
// only every second, since XInputGetState takes milliseconds to answer that
// nothing is there.
constexpr auto xinputPollPeriodMs = 4L;
constexpr auto xinputProbeInterval = 1.0;

// The Guide button, which the public header leaves out of wButtons.
constexpr auto xinputGuideButton = WORD {0x0400};

using XInputGetStateFunction = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputGetCapabilitiesFunction = DWORD(WINAPI*)(DWORD,
                                                     DWORD,
                                                     XINPUT_CAPABILITIES*);

// One GameInput's end of the process-wide feed. `wasAccepting` is the poll
// thread's alone.
struct XInputSink
{
    GameInputQueue* queue = nullptr;
    const std::atomic<bool>* active = nullptr;
    bool wasAccepting = false;

    bool accepting() const { return active->load(std::memory_order_relaxed); }
};

using Sink = std::shared_ptr<XInputSink>;

// One of XInput's four user slots, as the poll thread last saw it.
struct XInputSlot
{
    bool connected = false;
    int id = -1;
    DWORD packet = 0;
    XINPUT_GAMEPAD gamepad {};
    double nextProbe = 0.0;
};

struct XInputPoll
{
    bool polled = false;
    bool present = false;
    XINPUT_STATE state {};
};

struct XInputLibrary
{
    HMODULE module = nullptr;
    XInputGetStateFunction getState = nullptr;
    XInputGetCapabilitiesFunction getCapabilities = nullptr;

    bool isLoaded() const { return getState != nullptr; }

    // Ordinal 100 is XInputGetState with the Guide button reported, which the
    // named export masks off; xinput9_1_0 has only the named one.
    static XInputLibrary load()
    {
        for (auto* name: {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
        {
            auto library = XInputLibrary {LoadLibraryW(name)};

            if (library.module == nullptr)
                continue;

            library.getState = procAddress<XInputGetStateFunction>(
                library.module, MAKEINTRESOURCEA(100));

            if (library.getState == nullptr)
                library.getState = procAddress<XInputGetStateFunction>(
                    library.module, "XInputGetState");

            library.getCapabilities = procAddress<XInputGetCapabilitiesFunction>(
                library.module, "XInputGetCapabilities");

            if (library.getState != nullptr)
                return library;

            FreeLibrary(library.module);
        }

        return {};
    }

    void unload()
    {
        if (module != nullptr)
            FreeLibrary(module);

        module = nullptr;
        getState = nullptr;
        getCapabilities = nullptr;
    }

    const char* subTypeName(DWORD user) const
    {
        auto capabilities = XINPUT_CAPABILITIES {};

        if (getCapabilities == nullptr
            || getCapabilities(user, XINPUT_FLAG_GAMEPAD, &capabilities)
                   != ERROR_SUCCESS)
            return "unknown";

        switch (capabilities.SubType)
        {
            case XINPUT_DEVSUBTYPE_GAMEPAD:
                return "gamepad";
            case XINPUT_DEVSUBTYPE_WHEEL:
                return "wheel";
            case XINPUT_DEVSUBTYPE_ARCADE_STICK:
                return "arcade stick";
            case XINPUT_DEVSUBTYPE_FLIGHT_STICK:
                return "flight stick";
            case XINPUT_DEVSUBTYPE_DANCE_PAD:
                return "dance pad";
            case XINPUT_DEVSUBTYPE_GUITAR:
            case XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE:
            case XINPUT_DEVSUBTYPE_GUITAR_BASS:
                return "guitar";
            case XINPUT_DEVSUBTYPE_DRUM_KIT:
                return "drum kit";
            case XINPUT_DEVSUBTYPE_ARCADE_PAD:
                return "arcade pad";
            default:
                return "unknown";
        }
    }
};

float xinputStickAxis(SHORT value)
{
    return std::clamp((float) value / 32767.0f, -1.0f, 1.0f);
}

float xinputTriggerAxis(BYTE value)
{
    return (float) value / 255.0f;
}

void pushXInputButton(GameInputQueue& queue,
                      int id,
                      GamepadButton button,
                      WORD buttons,
                      WORD mask,
                      double time)
{
    queue.gamepadButtonChanged(id, button, (buttons & mask) != 0, time);
}

// The whole of a gamepad: the queue drops the buttons that did not change.
void pushXInputGamepad(GameInputQueue& queue,
                       int id,
                       const XINPUT_GAMEPAD& pad,
                       double time)
{
    using Button = GamepadButton;
    const auto buttons = pad.wButtons;

    pushXInputButton(queue, id, Button::South, buttons, XINPUT_GAMEPAD_A, time);
    pushXInputButton(queue, id, Button::East, buttons, XINPUT_GAMEPAD_B, time);
    pushXInputButton(queue, id, Button::West, buttons, XINPUT_GAMEPAD_X, time);
    pushXInputButton(queue, id, Button::North, buttons, XINPUT_GAMEPAD_Y, time);
    pushXInputButton(queue,
                     id,
                     Button::LeftShoulder,
                     buttons,
                     XINPUT_GAMEPAD_LEFT_SHOULDER,
                     time);
    pushXInputButton(queue,
                     id,
                     Button::RightShoulder,
                     buttons,
                     XINPUT_GAMEPAD_RIGHT_SHOULDER,
                     time);
    pushXInputButton(
        queue, id, Button::LeftStick, buttons, XINPUT_GAMEPAD_LEFT_THUMB, time);
    pushXInputButton(
        queue, id, Button::RightStick, buttons, XINPUT_GAMEPAD_RIGHT_THUMB, time);
    pushXInputButton(queue, id, Button::Start, buttons, XINPUT_GAMEPAD_START, time);
    pushXInputButton(queue, id, Button::Back, buttons, XINPUT_GAMEPAD_BACK, time);
    pushXInputButton(queue, id, Button::Home, buttons, xinputGuideButton, time);
    pushXInputButton(
        queue, id, Button::DpadUp, buttons, XINPUT_GAMEPAD_DPAD_UP, time);
    pushXInputButton(
        queue, id, Button::DpadDown, buttons, XINPUT_GAMEPAD_DPAD_DOWN, time);
    pushXInputButton(
        queue, id, Button::DpadLeft, buttons, XINPUT_GAMEPAD_DPAD_LEFT, time);
    pushXInputButton(
        queue, id, Button::DpadRight, buttons, XINPUT_GAMEPAD_DPAD_RIGHT, time);

    queue.gamepadAxisChanged(id, GamepadAxis::LeftX, xinputStickAxis(pad.sThumbLX));
    queue.gamepadAxisChanged(id, GamepadAxis::LeftY, xinputStickAxis(pad.sThumbLY));
    queue.gamepadAxisChanged(id, GamepadAxis::RightX, xinputStickAxis(pad.sThumbRX));
    queue.gamepadAxisChanged(id, GamepadAxis::RightY, xinputStickAxis(pad.sThumbRY));
    queue.gamepadAxisChanged(
        id, GamepadAxis::LeftTrigger, xinputTriggerAxis(pad.bLeftTrigger));
    queue.gamepadAxisChanged(
        id, GamepadAxis::RightTrigger, xinputTriggerAxis(pad.bRightTrigger));
}

HANDLE makeXInputTimer()
{
    auto* timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    if (timer == nullptr)
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

    if (timer == nullptr)
        return nullptr;

    auto due = LARGE_INTEGER {};
    due.QuadPart = -xinputPollPeriodMs * 10'000LL;
    SetWaitableTimer(timer, &due, xinputPollPeriodMs, nullptr, nullptr, FALSE);
    return timer;
}

// XInput is process-wide and poll-only, so the first GameInput starts the one
// poll thread and every GameInput's sink is fed from it; the last one to leave
// stops it. Main thread only, but for `tick`.
//
// Connections and disconnections reach every sink; buttons and axes only the
// accepting ones, and a sink that starts accepting again is handed the whole
// state of every pad, since GameInput released everything while it was not.
class XInputHub
{
public:
    // Null when no XInput could be loaded.
    static XInputHub* join(const Sink& sink)
    {
        if (instance == nullptr)
        {
            auto library = XInputLibrary::load();

            if (!library.isLoaded())
                return nullptr;

            instance = new XInputHub(library);
        }

        instance->add(sink);
        return instance;
    }

    static void leave(const Sink& sink)
    {
        if (instance == nullptr || !instance->remove(sink))
            return;

        delete instance;
        instance = nullptr;
    }

private:
    explicit XInputHub(XInputLibrary libraryToUse)
        : library(libraryToUse)
    {
        poller = std::thread([this] { run(); });
    }

    ~XInputHub()
    {
        SetEvent(stopEvent);
        poller.join();

        if (timer != nullptr)
            CloseHandle(timer);

        CloseHandle(stopEvent);
        library.unload();
    }

    void add(const Sink& sink)
    {
        auto lock = std::scoped_lock {mutex};
        const auto time = GameInputQueue::now();

        sinks.push_back(sink);
        sink->wasAccepting = sink->accepting();

        for (auto user = DWORD {0}; user < XUSER_MAX_COUNT; ++user)
            if (slots[user].connected)
                connect(*sink, user, time);

        ++joined;
    }

    // Once this returns nothing pushes into the sink's queue. True when it
    // was the last.
    bool remove(const Sink& sink)
    {
        auto lock = std::scoped_lock {mutex};
        std::erase(sinks, sink);
        return --joined == 0;
    }

    void run()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

        while (waitForTick())
            tick();
    }

    bool waitForTick()
    {
        if (timer == nullptr)
            return WaitForSingleObject(stopEvent, (DWORD) xinputPollPeriodMs)
                   == WAIT_TIMEOUT;

        HANDLE handles[] = {stopEvent, timer};
        return WaitForMultipleObjects(2, handles, FALSE, INFINITE)
               == WAIT_OBJECT_0 + 1;
    }

    // The states are read before the lock is taken: an empty slot can take
    // milliseconds to answer, and the main thread joining or leaving should
    // not wait on it.
    void tick()
    {
        const auto time = GameInputQueue::now();
        auto polls = Array<XInputPoll, XUSER_MAX_COUNT> {};

        for (auto user = DWORD {0}; user < XUSER_MAX_COUNT; ++user)
        {
            auto& slot = slots[user];

            if (!slot.connected && time < slot.nextProbe)
                continue;

            auto& poll = polls[user];
            poll.polled = true;
            poll.present = library.getState(user, &poll.state) == ERROR_SUCCESS;
        }

        auto lock = std::scoped_lock {mutex};

        for (auto& sink: sinks)
            resumeIfAccepting(*sink, time);

        for (auto user = DWORD {0}; user < XUSER_MAX_COUNT; ++user)
            if (polls[user].polled)
                apply(user, polls[user], time);
    }

    void resumeIfAccepting(XInputSink& sink, double time)
    {
        const auto accepting = sink.accepting();

        if (accepting && !sink.wasAccepting)
            for (auto& slot: slots)
                if (slot.connected)
                    pushXInputGamepad(*sink.queue, slot.id, slot.gamepad, time);

        sink.wasAccepting = accepting;
    }

    void apply(DWORD user, const XInputPoll& poll, double time)
    {
        auto& slot = slots[user];

        if (poll.present)
        {
            if (!slot.connected)
                attach(user, poll.state, time);
            else if (poll.state.dwPacketNumber != slot.packet)
                update(slot, poll.state, time);

            return;
        }

        if (slot.connected)
            detach(user, time);

        slot.nextProbe = time + xinputProbeInterval;
    }

    void attach(DWORD user, const XINPUT_STATE& state, double time)
    {
        auto& slot = slots[user];
        slot.connected = true;
        slot.id = nextGamepadId++;
        slot.packet = state.dwPacketNumber;
        slot.gamepad = state.Gamepad;

        LOG("GameInput: gamepad ",
            slot.id,
            " connected: XInput user ",
            user,
            " (",
            library.subTypeName(user),
            "), Xbox, player ",
            user);

        for (auto& sink: sinks)
        {
            connect(*sink, user, time);

            if (sink->accepting())
                pushXInputGamepad(*sink->queue, slot.id, slot.gamepad, time);
        }
    }

    void update(XInputSlot& slot, const XINPUT_STATE& state, double time)
    {
        slot.packet = state.dwPacketNumber;
        slot.gamepad = state.Gamepad;

        for (auto& sink: sinks)
            if (sink->accepting())
                pushXInputGamepad(*sink->queue, slot.id, slot.gamepad, time);
    }

    void detach(DWORD user, double time)
    {
        auto& slot = slots[user];

        LOG("GameInput: gamepad ", slot.id, " disconnected: XInput user ", user);

        for (auto& sink: sinks)
            sink->queue->gamepadDisconnected(slot.id, time);

        slot.connected = false;
        slot.id = -1;
        slot.gamepad = {};
    }

    // The user index is the light the controller shows, so it is the player.
    void connect(XInputSink& sink, DWORD user, double time)
    {
        sink.queue->gamepadConnected(
            slots[user].id, GamepadFamily::Xbox, (int) user, time);
    }

    static inline XInputHub* instance = nullptr;

    XInputLibrary library;
    std::mutex mutex;
    std::vector<Sink> sinks;
    Array<XInputSlot, XUSER_MAX_COUNT> slots {};
    int nextGamepadId = 0;
    int joined = 0;
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE timer = makeXInputTimer();
    std::thread poller;
};

// Keys and mouse stay the window's: XInput has neither.
struct XInputBackend final : GameInputBackend
{
    explicit XInputBackend(Sink sinkToUse)
        : sink(std::move(sinkToUse))
    {
    }

    ~XInputBackend() override { XInputHub::leave(sink); }

    bool ownsKeys() const override { return false; }
    bool ownsMouse() const override { return false; }

    Sink sink;
};
} // namespace

std::unique_ptr<GameInputBackend>
    makeGameInputBackend(GameInputQueue& queue, const std::atomic<bool>& active)
{
    auto sink = std::make_shared<XInputSink>();
    sink->queue = &queue;
    sink->active = &active;

    if (XInputHub::join(sink) == nullptr)
        return nullptr;

    return std::make_unique<XInputBackend>(std::move(sink));
}
} // namespace eacp::Graphics
