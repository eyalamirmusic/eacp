#include "GameInputQueue.h"

#include <bit>
#include <chrono>

namespace eacp::Graphics
{
namespace
{
void addTo(std::atomic<float>& total, float amount)
{
    auto current = total.load(std::memory_order_relaxed);

    while (!total.compare_exchange_weak(current, current + amount))
    {
    }
}

bool isValidButton(MouseButton button)
{
    return (int) button >= 0 && (int) button < GameInputFrame::buttonCount;
}

template <size_t Size>
bool applyEdge(std::bitset<Size>& down,
               std::bitset<Size>& edges,
               size_t index,
               bool pressed)
{
    if (down[index] == pressed)
        return false;

    down[index] = pressed;
    edges.set(index);
    return true;
}
} // namespace

GameInputQueue::GameInputQueue(int capacityToUse)
    : capacity(std::bit_ceil((std::size_t) std::max(capacityToUse, 2)))
    , mask(capacity - 1)
{
    slots = std::make_unique<Slot[]>(capacity);

    for (auto index = std::size_t {0}; index < capacity; ++index)
        slots[index].sequence.store(index, std::memory_order_relaxed);

    const auto synthesizedLimit =
        GameInputFrame::keyCount + GameInputFrame::buttonCount;
    frame.frameEvents.reserve((std::size_t) capacity + synthesizedLimit);
}

double GameInputQueue::now()
{
    const auto sinceEpoch = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(sinceEpoch).count();
}

void GameInputQueue::keyChanged(uint16_t key, bool down, double time)
{
    if (key >= GameInputFrame::keyCount)
        return;

    if (keysHeld[key].exchange(down) == down)
        return;

    const auto type = down ? InputEventType::KeyDown : InputEventType::KeyUp;
    enqueue({type, key, {}, time});
}

void GameInputQueue::mouseButtonChanged(MouseButton button, bool down, double time)
{
    if (!isValidButton(button))
        return;

    if (buttonsHeld[(int) button].exchange(down) == down)
        return;

    const auto type = down ? InputEventType::MouseDown : InputEventType::MouseUp;
    enqueue({type, (uint16_t) button, {}, time});
}

void GameInputQueue::mouseMoved(Point delta, double time)
{
    if (delta.x == 0.0f && delta.y == 0.0f)
        return;

    if (push({InputEventType::MouseMove, 0, delta, time}))
        return;

    addTo(lostDeltaX, delta.x);
    addTo(lostDeltaY, delta.y);
    overflowed.store(true);
}

void GameInputQueue::releaseAll(double time)
{
    for (auto key = 0; key < GameInputFrame::keyCount; ++key)
        if (keysHeld[key].exchange(false))
            enqueue({InputEventType::KeyUp, (uint16_t) key, {}, time});

    for (auto button = 0; button < GameInputFrame::buttonCount; ++button)
        if (buttonsHeld[button].exchange(false))
            enqueue({InputEventType::MouseUp, (uint16_t) button, {}, time});
}

void GameInputQueue::enqueue(const InputEvent& event)
{
    if (!push(event))
        overflowed.store(true);
}

bool GameInputQueue::push(const InputEvent& event)
{
    auto position = writePosition.load(std::memory_order_relaxed);

    while (true)
    {
        auto& slot = slots[position & mask];
        const auto sequence = slot.sequence.load(std::memory_order_acquire);
        const auto distance = (std::ptrdiff_t) sequence - (std::ptrdiff_t) position;

        if (distance == 0)
        {
            if (writePosition.compare_exchange_weak(
                    position, position + 1, std::memory_order_relaxed))
            {
                slot.event = event;
                slot.sequence.store(position + 1, std::memory_order_release);
                return true;
            }
        }
        else if (distance < 0)
        {
            return false;
        }
        else
        {
            position = writePosition.load(std::memory_order_relaxed);
        }
    }
}

bool GameInputQueue::pop(InputEvent& event)
{
    const auto position = readPosition.load(std::memory_order_relaxed);
    auto& slot = slots[position & mask];
    const auto sequence = slot.sequence.load(std::memory_order_acquire);

    if ((std::ptrdiff_t) sequence - (std::ptrdiff_t) (position + 1) < 0)
        return false;

    event = slot.event;
    slot.sequence.store(position + capacity, std::memory_order_release);
    readPosition.store(position + 1, std::memory_order_relaxed);
    return true;
}

const GameInputFrame& GameInputQueue::snapshot(double now)
{
    frame.keysPressed.reset();
    frame.keysReleased.reset();
    frame.buttonsPressed.reset();
    frame.buttonsReleased.reset();
    frame.delta = {};
    frame.frameEvents.clear();
    frame.snapshotTime = now;
    frame.newestTime = 0.0;

    auto event = InputEvent {};

    for (auto drained = std::size_t {0}; drained < capacity && pop(event); ++drained)
        apply(event);

    frame.dropped = overflowed.exchange(false);
    reconcile(now);

    const auto lost = takeLostDelta();
    frame.delta = frame.delta + lost;

    return frame;
}

void GameInputQueue::apply(const InputEvent& event)
{
    auto accepted = true;

    switch (event.type)
    {
        case InputEventType::KeyDown:
            accepted =
                applyEdge(frame.keysDown, frame.keysPressed, event.code, true);
            break;
        case InputEventType::KeyUp:
            accepted =
                applyEdge(frame.keysDown, frame.keysReleased, event.code, false);
            break;
        case InputEventType::MouseDown:
            accepted =
                applyEdge(frame.buttonsDown, frame.buttonsPressed, event.code, true);
            break;
        case InputEventType::MouseUp:
            accepted = applyEdge(
                frame.buttonsDown, frame.buttonsReleased, event.code, false);
            break;
        case InputEventType::MouseMove:
            frame.delta = frame.delta + event.delta;
            break;
    }

    if (!accepted)
        return;

    frame.frameEvents.add(event);
    frame.newestTime = std::max(frame.newestTime, event.timestamp);
}

void GameInputQueue::reconcile(double now)
{
    for (auto key = 0; key < GameInputFrame::keyCount; ++key)
    {
        const auto held = keysHeld[key].load(std::memory_order_relaxed);

        if (held != frame.keysDown[(size_t) key])
            apply({held ? InputEventType::KeyDown : InputEventType::KeyUp,
                   (uint16_t) key,
                   {},
                   now});
    }

    for (auto button = 0; button < GameInputFrame::buttonCount; ++button)
    {
        const auto held = buttonsHeld[button].load(std::memory_order_relaxed);

        if (held != frame.buttonsDown[(size_t) button])
            apply({held ? InputEventType::MouseDown : InputEventType::MouseUp,
                   (uint16_t) button,
                   {},
                   now});
    }
}

Point GameInputQueue::takeLostDelta()
{
    return {lostDeltaX.exchange(0.0f), lostDeltaY.exchange(0.0f)};
}
} // namespace eacp::Graphics
