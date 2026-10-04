#include "GameInput.h"
#include "GameInputBackend.h"
#include "../Window/Window.h"

namespace eacp::Graphics
{
namespace
{
std::unique_ptr<GameInputBackend> makeBackendFor(GameInputSource source,
                                                 GameInputQueue& queue,
                                                 const std::atomic<bool>& active)
{
    if (source == GameInputSource::WindowEvents)
        return nullptr;

    return makeGameInputBackend(queue, active);
}

bool isMotion(const MouseEvent& event)
{
    return event.type == MouseEventType::Moved
           || event.type == MouseEventType::Dragged;
}

bool isButton(const MouseEvent& event)
{
    return event.type == MouseEventType::Down || event.type == MouseEventType::Up;
}
} // namespace

GameInput::GameInput(Window& windowToUse, GameInputSource source)
    : window(windowToUse)
    , active(windowToUse.events.input.isActive())
    , backend(makeBackendFor(source, queue, active))
{
    window.events.input.addListener(*this);
}

GameInput::~GameInput()
{
    window.events.input.removeListener(*this);
    backend.reset();
}

double GameInput::now()
{
    return GameInputQueue::now();
}

const GameInputFrame& GameInput::snapshot()
{
    const auto time = now();

    if (backend != nullptr && !active.load())
        queue.releaseAll(time);

    return queue.snapshot(time);
}

std::string_view GameInput::backendName() const
{
    const auto keys = backendOwnsKeys();
    const auto mouse = backendOwnsMouse();

    if (keys && mouse)
        return "GameController";

    if (keys)
        return "GameController keys, window mouse";

    if (mouse)
        return "window keys, GameController mouse";

    return "Window events";
}

bool GameInput::backendOwnsKeys() const
{
    return backend != nullptr && backend->ownsKeys();
}

bool GameInput::backendOwnsMouse() const
{
    return backend != nullptr && backend->ownsMouse();
}

void GameInput::windowKeyEvent(const KeyEvent& event)
{
    if (backendOwnsKeys())
        return;

    queue.keyChanged(event.keyCode, event.type == KeyEventType::Down, now());
}

void GameInput::windowMouseEvent(const MouseEvent& event)
{
    if (backendOwnsMouse())
        return;

    if (isMotion(event))
        queue.mouseMoved(event.rawDelta, now());
    else if (isButton(event))
        queue.mouseButtonChanged(
            event.button, event.type == MouseEventType::Down, now());
}

void GameInput::windowActivationChanged(bool isKey)
{
    active.store(isKey);

    if (!isKey)
        queue.releaseAll(now());
}
} // namespace eacp::Graphics
