#pragma once

#include "../Primitives/Primitives.h"

#include <atomic>
#include <bitset>
#include <memory>

namespace eacp::Graphics
{

enum class MouseButton;

enum class InputEventType : uint8_t
{
    KeyDown,
    KeyUp,
    MouseDown,
    MouseUp,
    MouseMove,
    GamepadConnected,
    GamepadDisconnected,
    GamepadDown,
    GamepadUp
};

// Named by position, whatever the controller prints on them: South is A on an
// Xbox pad, cross on a PlayStation one and B on a Nintendo one.
enum class GamepadButton : uint8_t
{
    South,
    East,
    West,
    North,
    LeftShoulder,
    RightShoulder,
    LeftStick,
    RightStick,
    Start,
    Back,
    Home,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Count
};

// Sticks run -1..1 with y up, triggers 0..1, raw: no deadzone is applied.
enum class GamepadAxis : uint8_t
{
    LeftX,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,
    Count
};

// Which labels the controller wears, for showing the right button names.
enum class GamepadFamily : uint8_t
{
    Generic,
    Xbox,
    PlayStation,
    Nintendo
};

// One change of input state, stamped when it reached the process on
// GameInputQueue::now()'s clock.
struct InputEvent
{
    InputEventType type = InputEventType::KeyDown;

    // A KeyCode for the key events, a MouseButton for the mouse button events,
    // a GamepadButton for GamepadDown and GamepadUp, a GamepadFamily for
    // GamepadConnected.
    uint16_t code = 0;

    // MouseMove only: the device's own movement, unaccelerated, y down.
    Point delta;

    double timestamp = 0.0;

    // The gamepad events only: the id of the controller.
    int gamepad = -1;

    constexpr bool isKey() const
    {
        return type == InputEventType::KeyDown || type == InputEventType::KeyUp;
    }

    constexpr bool isGamepad() const { return gamepad >= 0; }
};

// One connected controller as a frame sees it: buttons with the same rules as
// keys, axes as their latest values.
class GamepadState
{
public:
    static constexpr int buttonCount = (int) GamepadButton::Count;
    static constexpr int axisCount = (int) GamepadAxis::Count;

    // Stable while the controller stays connected; not reused after.
    constexpr int id() const { return gamepadId; }

    // The player number the platform shows on the controller, from 0, or -1.
    constexpr int playerIndex() const { return player; }

    constexpr GamepadFamily family() const { return kind; }

    constexpr bool isDown(GamepadButton button) const
    {
        return inRange(button) && buttonsDown[(size_t) button];
    }

    constexpr bool wasPressed(GamepadButton button) const
    {
        return inRange(button) && buttonsPressed[(size_t) button];
    }

    constexpr bool wasReleased(GamepadButton button) const
    {
        return inRange(button) && buttonsReleased[(size_t) button];
    }

    constexpr float axis(GamepadAxis which) const
    {
        return (int) which < axisCount ? axes[(int) which] : 0.0f;
    }

    Point leftStick() const;
    Point rightStick() const;

private:
    friend class GameInputFrame;
    friend class GameInputQueue;

    static constexpr bool inRange(GamepadButton button)
    {
        return (int) button < buttonCount;
    }

    std::bitset<buttonCount> buttonsDown;
    std::bitset<buttonCount> buttonsPressed;
    std::bitset<buttonCount> buttonsReleased;
    Array<float, axisCount> axes {};
    int gamepadId = -1;
    int player = -1;
    GamepadFamily kind = GamepadFamily::Generic;
};

// The input a frame sees: the state after everything that arrived since the
// previous snapshot, the edges on the way there, and the events themselves in
// the order they arrived. A key pressed and released within one frame is both
// wasPressed and wasReleased, and not isDown.
class GameInputFrame
{
public:
    static constexpr int keyCount = 128;
    static constexpr int buttonCount = 4;
    static constexpr int maxGamepads = 8;

    constexpr bool isDown(uint16_t key) const
    {
        return inRange(key) && keysDown[key];
    }

    constexpr bool wasPressed(uint16_t key) const
    {
        return inRange(key) && keysPressed[key];
    }

    constexpr bool wasReleased(uint16_t key) const
    {
        return inRange(key) && keysReleased[key];
    }

    constexpr bool isMouseDown(MouseButton button) const
    {
        return buttonsDown[(size_t) button];
    }

    constexpr bool wasMousePressed(MouseButton button) const
    {
        return buttonsPressed[(size_t) button];
    }

    constexpr bool wasMouseReleased(MouseButton button) const
    {
        return buttonsReleased[(size_t) button];
    }

    // The device's movement since the previous snapshot, summed.
    constexpr Point mouseDelta() const { return delta; }

    // The connected controllers, in the order they connected.
    constexpr const Vector<GamepadState>& gamepads() const { return pads; }

    // Whether a controller connected or disconnected since the previous
    // snapshot.
    constexpr bool gamepadsChanged() const { return padsChanged; }

    constexpr const Vector<InputEvent>& events() const { return frameEvents; }

    // When the snapshot was taken, on GameInputQueue::now()'s clock.
    constexpr double time() const { return snapshotTime; }

    // The newest timestamp among events(), or 0 when there were none.
    constexpr double newestEventTime() const { return newestTime; }

    // Whether the queue overflowed since the previous snapshot. The state is
    // still right (it was reconciled against the producers' own), but some
    // edges or events in between are missing.
    constexpr bool droppedEvents() const { return dropped; }

private:
    friend class GameInputQueue;

    static constexpr bool inRange(uint16_t key) { return key < keyCount; }

    GamepadState* findGamepad(int id);

    std::bitset<keyCount> keysDown;
    std::bitset<keyCount> keysPressed;
    std::bitset<keyCount> keysReleased;
    std::bitset<buttonCount> buttonsDown;
    std::bitset<buttonCount> buttonsPressed;
    std::bitset<buttonCount> buttonsReleased;
    Point delta;
    Vector<GamepadState> pads;
    Vector<InputEvent> frameEvents;
    double snapshotTime = 0.0;
    double newestTime = 0.0;
    bool padsChanged = false;
    bool dropped = false;
};

// A bounded queue of InputEvents: any number of producer threads, one
// consumer. Lock-free (a Vyukov ring with a sequence number per slot) and
// allocation-free after construction on both sides.
//
// A full ring drops the newest event. Each producer call also records the
// true held state of its key or button before it pushes, and every snapshot
// reconciles against that, so the state stays right after an overflow or a
// race between producers; movement that did not fit is summed on the side and
// still reaches mouseDelta().
class GameInputQueue
{
public:
    static constexpr int defaultCapacity = 1024;

    explicit GameInputQueue(int capacity = defaultCapacity);

    // Producers, any thread. A press of a key already held is a repeat and
    // is ignored, as is a release of one that is not.
    void keyChanged(uint16_t key, bool down, double time);
    void mouseButtonChanged(MouseButton button, bool down, double time);
    void mouseMoved(Point delta, double time);

    // Gamepads, any thread, each named by an id >= 0 its producer picks and
    // does not reuse while it is connected. Up to GameInputFrame::maxGamepads
    // at once; the ones beyond are ignored. Connecting one already connected
    // updates its family and player index. Disconnecting releases its buttons
    // and zeroes its axes first. Axes are state, not events: the frame gets
    // the latest value of each.
    void
        gamepadConnected(int id, GamepadFamily family, int playerIndex, double time);
    void gamepadDisconnected(int id, double time);
    void gamepadButtonChanged(int id, GamepadButton button, bool down, double time);
    void gamepadAxisChanged(int id, GamepadAxis axis, float value);

    // Releases every key and button held, gamepad buttons included, as events
    // stamped `time`, and zeroes every gamepad axis. Gamepads stay connected.
    void releaseAll(double time);

    // The consumer, one thread: drains what arrived into the frame. The
    // reference stays valid, and is overwritten by the next call.
    //
    // The edges come from the events, but the held state always ends as the
    // producers' own: two producers racing on one key can enqueue in the
    // opposite order to their changes, so a key the events left in the wrong
    // state is corrected with a synthesized event stamped `now`, which is an
    // edge in this frame like any other.
    const GameInputFrame& snapshot(double now);

    // Seconds on the monotonic clock FrameTime is measured on
    // (std::chrono::steady_clock), from that clock's own epoch.
    static double now();

private:
    struct Slot
    {
        std::atomic<std::size_t> sequence {0};
        InputEvent event;
    };

    // A producer's side of one controller. `id` is claimed first (-1 is
    // free), then `connected` is set once family and player are stored.
    struct GamepadSlot
    {
        std::atomic<int> id {-1};
        std::atomic<bool> connected {false};
        std::atomic<GamepadFamily> family {GamepadFamily::Generic};
        std::atomic<int> playerIndex {-1};
        Array<std::atomic<bool>, GamepadState::buttonCount> buttonsHeld;
        Array<std::atomic<float>, GamepadState::axisCount> axes;
    };

    void enqueue(const InputEvent& event);
    bool push(const InputEvent& event);
    bool pop(InputEvent& event);
    void apply(const InputEvent& event);
    void applyGamepad(const InputEvent& event, bool& accepted);
    void reconcile(double now);
    void reconcileGamepads(double now);
    Point takeLostDelta();

    GamepadSlot* findGamepad(int id);
    GamepadSlot* claimGamepad(int id);
    void releaseGamepad(GamepadSlot& slot, int id, double time);

    std::unique_ptr<Slot[]> slots;
    std::size_t capacity = 0;
    std::size_t mask = 0;

    alignas(64) std::atomic<std::size_t> writePosition {0};
    alignas(64) std::atomic<std::size_t> readPosition {0};

    Array<std::atomic<bool>, GameInputFrame::keyCount> keysHeld;
    Array<std::atomic<bool>, GameInputFrame::buttonCount> buttonsHeld;
    Array<GamepadSlot, GameInputFrame::maxGamepads> gamepadSlots;
    std::atomic<bool> overflowed {false};
    std::atomic<float> lostDeltaX {0.0f};
    std::atomic<float> lostDeltaY {0.0f};

    GameInputFrame frame;
};

} // namespace eacp::Graphics
