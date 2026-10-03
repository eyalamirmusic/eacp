#pragma once

#include "GameInputQueue.h"

namespace eacp::Graphics
{

// A platform feed that pushes into a GameInput's queue from threads of its
// own. It drops what arrives while `active` is false, and once destroyed it
// pushes nothing more. Created and destroyed on the main thread.
struct GameInputBackend
{
    virtual ~GameInputBackend() = default;

    // Main thread: whether keys, and mouse, are arriving through this feed, so
    // the window's own events for them are to be ignored.
    virtual bool ownsKeys() const = 0;
    virtual bool ownsMouse() const = 0;
};

// Null where the platform has no such feed.
std::unique_ptr<GameInputBackend>
    makeGameInputBackend(GameInputQueue& queue, const std::atomic<bool>& active);

} // namespace eacp::Graphics
