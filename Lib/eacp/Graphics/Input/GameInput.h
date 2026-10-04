#pragma once

#include "GameInputQueue.h"
#include "../Window/WindowInput.h"

#include <string_view>

namespace eacp::Graphics
{

struct GameInputBackend;
class Window;

enum class GameInputSource
{
    // The platform's game input API where there is one (GameController on
    // Apple), the window's own events for whatever it does not cover.
    Automatic,

    // Only the window's own key and mouse events.
    WindowEvents
};

// Keyboard, mouse and gamepad state for a game loop, polled once a frame,
// beside the View key/mouse callbacks (which stay what UI and text entry use).
//
//     auto& frame = input.snapshot();   // once per frame, in update()
//     if (frame.isDown(KeyCode::W)) ...
//     if (frame.wasPressed(KeyCode::Space)) ...
//     yaw -= frame.mouseDelta().x * sensitivity;
//     for (auto& pad: frame.gamepads()) walk(pad.leftStick());
//
// Gamepads come only from the platform feed (GCController's extended gamepads
// on Apple); where there is none, frame.gamepads() stays empty.
//
// On Apple the events come from GameController (GCKeyboard, GCMouse) on a
// high-priority queue of their own, so they are captured and timestamped even
// while the main thread is busy drawing; elsewhere, and on Apple until a
// GameController device shows up, from the window's key and mouse events.
// Either way only while the window has key focus: losing it releases every
// key and button. Construct it on the main thread; the window must outlive it.
class GameInput : WindowInputListener
{
public:
    explicit GameInput(Window& windowToUse,
                       GameInputSource source = GameInputSource::Automatic);
    ~GameInput() override;

    GameInput(const GameInput&) = delete;
    GameInput& operator=(const GameInput&) = delete;

    // Drains everything that arrived since the previous call. The reference
    // stays valid until the next call.
    const GameInputFrame& snapshot();

    // Which feed is delivering keys and mouse right now.
    std::string_view backendName() const;

    // The clock every InputEvent and GameInputFrame::time() is on.
    static double now();

private:
    void windowKeyEvent(const KeyEvent& event) override;
    void windowMouseEvent(const MouseEvent& event) override;
    void windowActivationChanged(bool isKey) override;

    bool backendOwnsKeys() const;
    bool backendOwnsMouse() const;

    Window& window;
    GameInputQueue queue;
    std::atomic<bool> active {false};
    std::unique_ptr<GameInputBackend> backend;
};

} // namespace eacp::Graphics
