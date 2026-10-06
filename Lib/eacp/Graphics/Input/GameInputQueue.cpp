#include "GameInputQueue.h"
#include "../View/View.h"

#include <algorithm>
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

bool isValidGamepadButton(GamepadButton button)
{
    return (int) button < GamepadState::buttonCount;
}
} // namespace

Point GamepadState::leftStick() const
{
    return {axis(GamepadAxis::LeftX), axis(GamepadAxis::LeftY)};
}

Point GamepadState::rightStick() const
{
    return {axis(GamepadAxis::RightX), axis(GamepadAxis::RightY)};
}

GamepadState* GameInputFrame::findGamepad(int id)
{
    for (auto& pad: pads)
        if (pad.gamepadId == id)
            return &pad;

    return nullptr;
}

GameInputQueue::GameInputQueue(int capacityToUse)
    : capacity(std::bit_ceil((std::size_t) std::max(capacityToUse, 2)))
    , mask(capacity - 1)
{
    slots = std::make_unique<Slot[]>(capacity);

    for (auto index = std::size_t {0}; index < capacity; ++index)
        slots[index].sequence.store(index, std::memory_order_relaxed);

    const auto synthesizedLimit =
        GameInputFrame::keyCount + GameInputFrame::buttonCount
        + 2 * GameInputFrame::maxGamepads * (GamepadState::buttonCount + 2);
    frame.frameEvents.reserve((std::size_t) capacity + synthesizedLimit);
    frame.pads.reserve(2 * GameInputFrame::maxGamepads);
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

void GameInputQueue::gamepadConnected(int id,
                                      GamepadFamily family,
                                      int playerIndex,
                                      double time)
{
    if (id < 0)
        return;

    auto* slot = findGamepad(id);

    if (slot == nullptr)
        slot = claimGamepad(id);

    if (slot == nullptr)
        return;

    slot->family.store(family);
    slot->playerIndex.store(playerIndex);

    if (slot->connected.exchange(true))
        return;

    enqueue({InputEventType::GamepadConnected, (uint16_t) family, {}, time, id});
}

void GameInputQueue::gamepadDisconnected(int id, double time)
{
    auto* slot = findGamepad(id);

    if (slot == nullptr || !slot->connected.load())
        return;

    releaseGamepad(*slot, id, time);
    slot->connected.store(false);
    enqueue({InputEventType::GamepadDisconnected, 0, {}, time, id});
    slot->id.store(-1);
}

void GameInputQueue::gamepadButtonChanged(int id,
                                          GamepadButton button,
                                          bool down,
                                          double time)
{
    if (!isValidGamepadButton(button))
        return;

    auto* slot = findGamepad(id);

    if (slot == nullptr || !slot->connected.load())
        return;

    if (slot->buttonsHeld[(int) button].exchange(down) == down)
        return;

    const auto type = down ? InputEventType::GamepadDown : InputEventType::GamepadUp;
    enqueue({type, (uint16_t) button, {}, time, id});
}

void GameInputQueue::gamepadAxisChanged(int id, GamepadAxis axis, float value)
{
    if ((int) axis >= GamepadState::axisCount)
        return;

    auto* slot = findGamepad(id);

    if (slot != nullptr && slot->connected.load())
        slot->axes[(int) axis].store(value, std::memory_order_relaxed);
}

GameInputQueue::GamepadSlot* GameInputQueue::findGamepad(int id)
{
    if (id < 0)
        return nullptr;

    for (auto& slot: gamepadSlots)
        if (slot.id.load() == id)
            return &slot;

    return nullptr;
}

GameInputQueue::GamepadSlot* GameInputQueue::claimGamepad(int id)
{
    for (auto& slot: gamepadSlots)
    {
        auto free = -1;

        if (slot.id.compare_exchange_strong(free, id))
            return &slot;
    }

    return nullptr;
}

void GameInputQueue::releaseGamepad(GamepadSlot& slot, int id, double time)
{
    for (auto button = 0; button < GamepadState::buttonCount; ++button)
        if (slot.buttonsHeld[(int) button].exchange(false))
            enqueue({InputEventType::GamepadUp, (uint16_t) button, {}, time, id});

    for (auto& axis: slot.axes)
        axis.store(0.0f, std::memory_order_relaxed);
}

void GameInputQueue::releaseAll(double time)
{
    for (auto key = 0; key < GameInputFrame::keyCount; ++key)
        if (keysHeld[key].exchange(false))
            enqueue({InputEventType::KeyUp, (uint16_t) key, {}, time});

    for (auto button = 0; button < GameInputFrame::buttonCount; ++button)
        if (buttonsHeld[button].exchange(false))
            enqueue({InputEventType::MouseUp, (uint16_t) button, {}, time});

    for (auto& slot: gamepadSlots)
    {
        const auto id = slot.id.load();

        if (id >= 0 && slot.connected.load())
            releaseGamepad(slot, id, time);
    }
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
    frame.padsChanged = false;

    for (auto& pad: frame.pads)
    {
        pad.buttonsPressed.reset();
        pad.buttonsReleased.reset();
    }

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
        case InputEventType::GamepadConnected:
        case InputEventType::GamepadDisconnected:
        case InputEventType::GamepadDown:
        case InputEventType::GamepadUp:
            applyGamepad(event, accepted);
            break;
    }

    if (!accepted)
        return;

    frame.frameEvents.add(event);
    frame.newestTime = std::max(frame.newestTime, event.timestamp);
}

void GameInputQueue::applyGamepad(const InputEvent& event, bool& accepted)
{
    auto* pad = frame.findGamepad(event.gamepad);

    if (event.type == InputEventType::GamepadConnected)
    {
        accepted = pad == nullptr;

        if (!accepted)
            return;

        auto added = GamepadState {};
        added.gamepadId = event.gamepad;
        added.kind = (GamepadFamily) event.code;
        frame.pads.add(added);
        frame.padsChanged = true;
        return;
    }

    accepted = pad != nullptr;

    if (!accepted)
        return;

    switch (event.type)
    {
        case InputEventType::GamepadDisconnected:
            std::erase_if(frame.pads.getVector(),
                          [&](const GamepadState& candidate)
                          { return candidate.gamepadId == event.gamepad; });
            frame.padsChanged = true;
            break;
        case InputEventType::GamepadDown:
            accepted =
                applyEdge(pad->buttonsDown, pad->buttonsPressed, event.code, true);
            break;
        default:
            accepted =
                applyEdge(pad->buttonsDown, pad->buttonsReleased, event.code, false);
            break;
    }
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

    reconcileGamepads(now);
}

// Pads the producers no longer have are disconnected, pads they have and the
// frame lacks are connected, then each one's buttons end as held and its axes
// take their latest values.
void GameInputQueue::reconcileGamepads(double now)
{
    auto isConnected = [this](int id)
    {
        for (auto& slot: gamepadSlots)
            if (slot.connected.load() && slot.id.load() == id)
                return true;

        return false;
    };

    for (auto index = frame.pads.size(); index-- > 0;)
    {
        const auto id = frame.pads[index].gamepadId;

        if (!isConnected(id))
            apply({InputEventType::GamepadDisconnected, 0, {}, now, id});
    }

    for (auto& slot: gamepadSlots)
    {
        if (!slot.connected.load())
            continue;

        const auto id = slot.id.load();

        if (id < 0)
            continue;

        const auto family = slot.family.load();

        if (frame.findGamepad(id) == nullptr)
            apply(
                {InputEventType::GamepadConnected, (uint16_t) family, {}, now, id});

        auto* pad = frame.findGamepad(id);
        pad->kind = family;
        pad->player = slot.playerIndex.load();

        for (auto button = 0; button < GamepadState::buttonCount; ++button)
        {
            const auto held = slot.buttonsHeld[(int) button].load();

            if (held != pad->buttonsDown[(size_t) button])
                apply(
                    {held ? InputEventType::GamepadDown : InputEventType::GamepadUp,
                     (uint16_t) button,
                     {},
                     now,
                     id});
        }

        for (auto axis = 0; axis < GamepadState::axisCount; ++axis)
            pad->axes[(int) axis] =
                slot.axes[(int) axis].load(std::memory_order_relaxed);
    }
}

Point GameInputQueue::takeLostDelta()
{
    return {lostDeltaX.exchange(0.0f), lostDeltaY.exchange(0.0f)};
}
} // namespace eacp::Graphics
