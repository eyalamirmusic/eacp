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
// A connected slot is read every tick; an empty one only every second, since
// XInputGetState takes milliseconds to answer that nothing is there.
constexpr auto xinputPollPeriodMs = 4L;
constexpr auto xinputProbeIntervalSeconds = 1.0;

// The Guide button, which the public header leaves out of wButtons.
constexpr auto xinputGuideButton = WORD {0x0400};

using XInputGetStateFunction = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

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
    double nextProbeSeconds = 0.0;
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

    bool isLoaded() const { return getState != nullptr; }
};

// Ordinal 100 is XInputGetState with the Guide button reported, which the
// named export masks off.
XInputLibrary loadXInputLibrary()
{
    auto library = XInputLibrary {LoadLibraryW(L"xinput1_4.dll")};

    if (library.module == nullptr)
        return {};

    library.getState =
        procAddress<XInputGetStateFunction>(library.module, MAKEINTRESOURCEA(100));

    if (library.getState == nullptr)
        library.getState =
            procAddress<XInputGetStateFunction>(library.module, "XInputGetState");

    return library;
}

float stickAxisValue(SHORT value)
{
    return std::clamp((float) value / 32767.0f, -1.0f, 1.0f);
}

float triggerAxisValue(BYTE value)
{
    return (float) value / 255.0f;
}

void pushButtonState(GameInputQueue& queue,
                     int id,
                     GamepadButton button,
                     WORD buttons,
                     WORD mask,
                     double timestampSeconds)
{
    queue.gamepadButtonChanged(id, button, (buttons & mask) != 0, timestampSeconds);
}

// Every button and axis of the pad as XInput last reported it, stamped with
// when that report was read. The queue drops the buttons that did not change.
void pushWholeGamepadState(GameInputQueue& queue,
                           int id,
                           const XINPUT_GAMEPAD& pad,
                           double timestampSeconds)
{
    using Button = GamepadButton;
    const auto buttons = pad.wButtons;
    const auto push = [&](Button button, WORD mask)
    { pushButtonState(queue, id, button, buttons, mask, timestampSeconds); };

    push(Button::South, XINPUT_GAMEPAD_A);
    push(Button::East, XINPUT_GAMEPAD_B);
    push(Button::West, XINPUT_GAMEPAD_X);
    push(Button::North, XINPUT_GAMEPAD_Y);
    push(Button::LeftShoulder, XINPUT_GAMEPAD_LEFT_SHOULDER);
    push(Button::RightShoulder, XINPUT_GAMEPAD_RIGHT_SHOULDER);
    push(Button::LeftStick, XINPUT_GAMEPAD_LEFT_THUMB);
    push(Button::RightStick, XINPUT_GAMEPAD_RIGHT_THUMB);
    push(Button::Start, XINPUT_GAMEPAD_START);
    push(Button::Back, XINPUT_GAMEPAD_BACK);
    push(Button::Home, xinputGuideButton);
    push(Button::DpadUp, XINPUT_GAMEPAD_DPAD_UP);
    push(Button::DpadDown, XINPUT_GAMEPAD_DPAD_DOWN);
    push(Button::DpadLeft, XINPUT_GAMEPAD_DPAD_LEFT);
    push(Button::DpadRight, XINPUT_GAMEPAD_DPAD_RIGHT);

    queue.gamepadAxisChanged(id, GamepadAxis::LeftX, stickAxisValue(pad.sThumbLX));
    queue.gamepadAxisChanged(id, GamepadAxis::LeftY, stickAxisValue(pad.sThumbLY));
    queue.gamepadAxisChanged(id, GamepadAxis::RightX, stickAxisValue(pad.sThumbRX));
    queue.gamepadAxisChanged(id, GamepadAxis::RightY, stickAxisValue(pad.sThumbRY));
    queue.gamepadAxisChanged(
        id, GamepadAxis::LeftTrigger, triggerAxisValue(pad.bLeftTrigger));
    queue.gamepadAxisChanged(
        id, GamepadAxis::RightTrigger, triggerAxisValue(pad.bRightTrigger));
}

HANDLE makePollTimer()
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
    // Null when XInput could not be loaded.
    static XInputHub* join(const Sink& sink)
    {
        if (instance == nullptr)
        {
            auto library = loadXInputLibrary();

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
        FreeLibrary(library.module);
    }

    void add(const Sink& sink)
    {
        auto lock = std::scoped_lock {mutex};
        const auto timestampSeconds = GameInputQueue::now();

        sinks.push_back(sink);
        sink->wasAccepting = sink->accepting();

        for (auto user = DWORD {0}; user < XUSER_MAX_COUNT; ++user)
            if (slots[user].connected)
                connect(*sink, user, timestampSeconds);

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
        const auto timestampSeconds = GameInputQueue::now();
        auto polls = Array<XInputPoll, XUSER_MAX_COUNT> {};

        for (auto user = DWORD {0}; user < XUSER_MAX_COUNT; ++user)
        {
            auto& slot = slots[user];

            if (!slot.connected && timestampSeconds < slot.nextProbeSeconds)
                continue;

            auto& poll = polls[user];
            poll.polled = true;
            poll.present = library.getState(user, &poll.state) == ERROR_SUCCESS;
        }

        auto lock = std::scoped_lock {mutex};

        for (auto& sink: sinks)
            resumeIfAccepting(*sink, timestampSeconds);

        for (auto user = DWORD {0}; user < XUSER_MAX_COUNT; ++user)
            if (polls[user].polled)
                apply(user, polls[user], timestampSeconds);
    }

    void resumeIfAccepting(XInputSink& sink, double timestampSeconds)
    {
        const auto accepting = sink.accepting();

        if (accepting && !sink.wasAccepting)
            for (auto& slot: slots)
                if (slot.connected)
                    pushWholeGamepadState(
                        *sink.queue, slot.id, slot.gamepad, timestampSeconds);

        sink.wasAccepting = accepting;
    }

    void apply(DWORD user, const XInputPoll& poll, double timestampSeconds)
    {
        auto& slot = slots[user];

        if (poll.present)
        {
            if (!slot.connected)
                attach(user, poll.state, timestampSeconds);
            else if (poll.state.dwPacketNumber != slot.packet)
                update(slot, poll.state, timestampSeconds);

            return;
        }

        if (slot.connected)
            detach(user, timestampSeconds);

        slot.nextProbeSeconds = timestampSeconds + xinputProbeIntervalSeconds;
    }

    void attach(DWORD user, const XINPUT_STATE& state, double timestampSeconds)
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
            ", Xbox, player ",
            user);

        for (auto& sink: sinks)
        {
            connect(*sink, user, timestampSeconds);

            if (sink->accepting())
                pushWholeGamepadState(
                    *sink->queue, slot.id, slot.gamepad, timestampSeconds);
        }
    }

    void update(XInputSlot& slot, const XINPUT_STATE& state, double timestampSeconds)
    {
        slot.packet = state.dwPacketNumber;
        slot.gamepad = state.Gamepad;

        for (auto& sink: sinks)
            if (sink->accepting())
                pushWholeGamepadState(
                    *sink->queue, slot.id, slot.gamepad, timestampSeconds);
    }

    void detach(DWORD user, double timestampSeconds)
    {
        auto& slot = slots[user];

        LOG("GameInput: gamepad ", slot.id, " disconnected: XInput user ", user);

        for (auto& sink: sinks)
            sink->queue->gamepadDisconnected(slot.id, timestampSeconds);

        slot.connected = false;
        slot.id = -1;
        slot.gamepad = {};
    }

    // The user index is the light the controller shows, so it is the player.
    void connect(XInputSink& sink, DWORD user, double timestampSeconds)
    {
        sink.queue->gamepadConnected(
            slots[user].id, GamepadFamily::Xbox, (int) user, timestampSeconds);
    }

    static inline XInputHub* instance = nullptr;

    XInputLibrary library;
    std::mutex mutex;
    std::vector<Sink> sinks;
    Array<XInputSlot, XUSER_MAX_COUNT> slots {};
    int nextGamepadId = 0;
    int joined = 0;
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE timer = makePollTimer();
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
