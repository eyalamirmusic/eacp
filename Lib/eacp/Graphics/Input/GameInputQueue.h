#pragma once

#include "../View/View.h"

#include <atomic>
#include <bitset>
#include <memory>

namespace eacp::Graphics
{

enum class InputEventType : uint8_t
{
    KeyDown,
    KeyUp,
    MouseDown,
    MouseUp,
    MouseMove
};

// One change of input state, stamped when it reached the process on
// GameInputQueue::now()'s clock.
struct InputEvent
{
    InputEventType type = InputEventType::KeyDown;

    // A KeyCode for the key events, a MouseButton for the button events.
    uint16_t code = 0;

    // MouseMove only: the device's own movement, unaccelerated, y down.
    Point delta;

    double timestamp = 0.0;

    bool isKey() const
    {
        return type == InputEventType::KeyDown || type == InputEventType::KeyUp;
    }
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

    bool isDown(uint16_t key) const { return inRange(key) && keysDown[key]; }
    bool wasPressed(uint16_t key) const { return inRange(key) && keysPressed[key]; }

    bool wasReleased(uint16_t key) const
    {
        return inRange(key) && keysReleased[key];
    }

    bool isMouseDown(MouseButton button) const
    {
        return buttonsDown[(size_t) button];
    }

    bool wasMousePressed(MouseButton button) const
    {
        return buttonsPressed[(size_t) button];
    }

    bool wasMouseReleased(MouseButton button) const
    {
        return buttonsReleased[(size_t) button];
    }

    // The device's movement since the previous snapshot, summed.
    Point mouseDelta() const { return delta; }

    const Vector<InputEvent>& events() const { return frameEvents; }

    // When the snapshot was taken, on GameInputQueue::now()'s clock.
    double time() const { return snapshotTime; }

    // The newest timestamp among events(), or 0 when there were none.
    double newestEventTime() const { return newestTime; }

    // Whether the queue overflowed since the previous snapshot. The state is
    // still right (it was reconciled against the producers' own), but some
    // edges or events in between are missing.
    bool droppedEvents() const { return dropped; }

private:
    friend class GameInputQueue;

    static bool inRange(uint16_t key) { return key < keyCount; }

    std::bitset<keyCount> keysDown;
    std::bitset<keyCount> keysPressed;
    std::bitset<keyCount> keysReleased;
    std::bitset<buttonCount> buttonsDown;
    std::bitset<buttonCount> buttonsPressed;
    std::bitset<buttonCount> buttonsReleased;
    Point delta;
    Vector<InputEvent> frameEvents;
    double snapshotTime = 0.0;
    double newestTime = 0.0;
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

    // Releases every key and button held, as events stamped `time`.
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

    void enqueue(const InputEvent& event);
    bool push(const InputEvent& event);
    bool pop(InputEvent& event);
    void apply(const InputEvent& event);
    void reconcile(double now);
    Point takeLostDelta();

    std::unique_ptr<Slot[]> slots;
    std::size_t capacity = 0;
    std::size_t mask = 0;

    alignas(64) std::atomic<std::size_t> writePosition {0};
    alignas(64) std::atomic<std::size_t> readPosition {0};

    Array<std::atomic<bool>, GameInputFrame::keyCount> keysHeld;
    Array<std::atomic<bool>, GameInputFrame::buttonCount> buttonsHeld;
    std::atomic<bool> overflowed {false};
    std::atomic<float> lostDeltaX {0.0f};
    std::atomic<float> lostDeltaY {0.0f};

    GameInputFrame frame;
};

} // namespace eacp::Graphics
