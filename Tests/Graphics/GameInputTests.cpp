#include "Common.h"

#include <eacp/Graphics/Input/HidKeyCodes.h>

#include <thread>

using namespace nano;
using namespace eacp::Graphics;

namespace
{
using eacp::Array;

KeyEvent keyEvent(uint16_t keyCode, KeyEventType type, bool isRepeat = false)
{
    auto event = KeyEvent {};
    event.keyCode = keyCode;
    event.type = type;
    event.isRepeat = isRepeat;
    return event;
}

MouseEvent mouseEvent(MouseEventType type, Point rawDelta = {})
{
    auto event = MouseEvent {};
    event.type = type;
    event.rawDelta = rawDelta;
    return event;
}
} // namespace

auto tHeldKeyHasOneEdge = test("GameInput/aHeldKeyIsPressedOnceThenJustDown") = []
{
    auto queue = GameInputQueue {};

    queue.keyChanged(KeyCode::W, true, 1.0);
    const auto& first = queue.snapshot(1.1);

    check(first.isDown(KeyCode::W));
    check(first.wasPressed(KeyCode::W));
    check(!first.wasReleased(KeyCode::W));

    queue.keyChanged(KeyCode::W, true, 1.2);
    const auto& second = queue.snapshot(1.3);

    check(second.isDown(KeyCode::W));
    check(!second.wasPressed(KeyCode::W));
    check(second.events().empty());

    queue.keyChanged(KeyCode::W, false, 1.4);
    const auto& third = queue.snapshot(1.5);

    check(!third.isDown(KeyCode::W));
    check(third.wasReleased(KeyCode::W));
    check(!third.wasPressed(KeyCode::W));
};

auto tTapWithinOneFrame =
    test("GameInput/aTapWithinOneFrameIsPressedAndReleased") = []
{
    auto queue = GameInputQueue {};

    queue.keyChanged(KeyCode::Space, true, 2.0);
    queue.keyChanged(KeyCode::Space, false, 2.01);
    const auto& frame = queue.snapshot(2.02);

    check(frame.wasPressed(KeyCode::Space));
    check(frame.wasReleased(KeyCode::Space));
    check(!frame.isDown(KeyCode::Space));
    check(frame.events().size() == 2);
    check(frame.events()[0].type == InputEventType::KeyDown);
    check(frame.events()[1].type == InputEventType::KeyUp);
    check(frame.newestEventTime() == 2.01);
    check(frame.time() == 2.02);
};

auto tReleaseAll = test("GameInput/releaseAllLetsGoOfEveryKeyAndButton") = []
{
    auto queue = GameInputQueue {};

    queue.keyChanged(KeyCode::A, true, 1.0);
    queue.keyChanged(KeyCode::Shift, true, 1.0);
    queue.mouseButtonChanged(MouseButton::Left, true, 1.0);
    queue.snapshot(1.1);

    queue.releaseAll(1.2);
    const auto& frame = queue.snapshot(1.3);

    check(!frame.isDown(KeyCode::A));
    check(!frame.isDown(KeyCode::Shift));
    check(!frame.isMouseDown(MouseButton::Left));
    check(frame.wasReleased(KeyCode::A));
    check(frame.wasReleased(KeyCode::Shift));
    check(frame.wasMouseReleased(MouseButton::Left));
    check(frame.events().size() == 3);
};

auto tMouseDeltaAccumulates = test("GameInput/mouseDeltaSumsWithinAFrame") = []
{
    auto queue = GameInputQueue {};

    queue.mouseMoved({1.0f, 2.0f}, 1.0);
    queue.mouseMoved({3.0f, -1.0f}, 1.001);
    queue.mouseMoved({-0.5f, 0.5f}, 1.002);

    const auto& frame = queue.snapshot(1.01);
    check(frame.mouseDelta().x == 3.5f);
    check(frame.mouseDelta().y == 1.5f);
    check(frame.events().size() == 3);

    const auto& next = queue.snapshot(1.02);
    check(next.mouseDelta().x == 0.0f);
    check(next.mouseDelta().y == 0.0f);
};

auto tMouseButtons = test("GameInput/mouseButtonsHaveEdges") = []
{
    auto queue = GameInputQueue {};

    queue.mouseButtonChanged(MouseButton::Right, true, 1.0);
    const auto& frame = queue.snapshot(1.1);

    check(frame.isMouseDown(MouseButton::Right));
    check(frame.wasMousePressed(MouseButton::Right));
    check(!frame.isMouseDown(MouseButton::Left));
};

auto tOverflowKeepsState = test("GameInput/anOverflowKeepsKeyStateRight") = []
{
    auto queue = GameInputQueue {4};

    for (auto index = 0; index < 20; ++index)
        queue.mouseMoved({1.0f, 0.0f}, 1.0);

    queue.keyChanged(KeyCode::D, true, 1.1);
    queue.keyChanged(KeyCode::A, true, 1.1);
    queue.keyChanged(KeyCode::A, false, 1.2);

    const auto& frame = queue.snapshot(1.3);

    check(frame.droppedEvents());
    check(frame.isDown(KeyCode::D));
    check(!frame.isDown(KeyCode::A));
    check(frame.mouseDelta().x == 20.0f);

    queue.keyChanged(KeyCode::D, false, 1.4);
    const auto& next = queue.snapshot(1.5);

    check(!next.droppedEvents());
    check(!next.isDown(KeyCode::D));
    check(next.wasReleased(KeyCode::D));
};

auto tUnknownKeysIgnored = test("GameInput/keysOutsideTheTableAreIgnored") = []
{
    auto queue = GameInputQueue {};

    queue.keyChanged(KeyCode::Unknown, true, 1.0);
    const auto& frame = queue.snapshot(1.1);

    check(frame.events().empty());
    check(!frame.isDown(KeyCode::Unknown));
};

auto tManyProducers = test("GameInput/manyProducerThreadsLoseNoMovement") = []
{
    constexpr auto threadCount = 4;
    constexpr auto movesPerThread = 20000;

    auto queue = GameInputQueue {64};
    auto finished = std::atomic<int> {0};
    auto total = 0.0f;

    auto produce = [&queue, &finished](int threadIndex)
    {
        const auto key = (uint16_t) (KeyCode::A + threadIndex);

        for (auto index = 0; index < movesPerThread; ++index)
        {
            queue.mouseMoved({1.0f, 0.0f}, GameInputQueue::now());

            if (index % 100 == 0)
                queue.keyChanged(key, (index / 100) % 2 == 0, GameInputQueue::now());
        }

        queue.keyChanged(key, true, GameInputQueue::now());
        ++finished;
    };

    auto threads = Array<std::thread, threadCount> {};

    for (auto index = 0; index < threadCount; ++index)
        threads[index] = std::thread(produce, index);

    while (finished.load() < threadCount)
        total += queue.snapshot(GameInputQueue::now()).mouseDelta().x;

    for (auto& thread: threads)
        thread.join();

    const auto& last = queue.snapshot(GameInputQueue::now());
    total += last.mouseDelta().x;

    check(total == (float) (threadCount * movesPerThread));

    for (auto index = 0; index < threadCount; ++index)
        check(last.isDown((uint16_t) (KeyCode::A + index)));
};

auto tSnapshotFollowsHeldState =
    test("GameInput/theSnapshotAlwaysEndsInTheHeldState") = []
{
    auto queue = GameInputQueue {};

    queue.keyChanged(KeyCode::W, true, 1.0);
    queue.releaseAll(1.1);
    queue.keyChanged(KeyCode::W, true, 1.2);
    const auto& pressedAgain = queue.snapshot(1.3);

    check(pressedAgain.isDown(KeyCode::W));
    check(pressedAgain.wasPressed(KeyCode::W));
    check(pressedAgain.wasReleased(KeyCode::W));
    check(pressedAgain.events().size() == 3);

    queue.releaseAll(1.4);
    const auto& released = queue.snapshot(1.5);

    check(!released.isDown(KeyCode::W));
    check(released.wasReleased(KeyCode::W));
    check(!released.wasPressed(KeyCode::W));

    queue.releaseAll(1.6);
    queue.keyChanged(KeyCode::W, false, 1.7);
    const auto& idle = queue.snapshot(1.8);

    check(!idle.isDown(KeyCode::W));
    check(!idle.wasReleased(KeyCode::W));
    check(idle.events().empty());
};

auto tRacingProducersLeaveNothingStuck =
    test("GameInput/racingProducersOnOneKeyLeaveNothingStuck") = []
{
    constexpr auto threadCount = 3;
    constexpr auto rounds = 300;
    constexpr auto togglesPerThread = 200;

    auto queue = GameInputQueue {};
    auto stuckRounds = 0;

    for (auto round = 0; round < rounds; ++round)
    {
        auto started = std::atomic<int> {0};
        auto finished = std::atomic<int> {0};

        auto produce = [&queue, &started, &finished]
        {
            ++started;

            while (started.load() <= threadCount)
            {
            }

            for (auto index = 0; index < togglesPerThread; ++index)
            {
                const auto down = index % 2 == 0;
                const auto time = GameInputQueue::now();
                queue.keyChanged(KeyCode::W, down, time);
                queue.mouseButtonChanged(MouseButton::Left, down, time);
            }

            ++finished;
        };

        auto threads = Array<std::thread, threadCount> {};

        for (auto& thread: threads)
            thread = std::thread(produce);

        while (started.load() < threadCount)
        {
        }

        ++started;

        while (finished.load() < threadCount)
        {
            queue.releaseAll(GameInputQueue::now());
            queue.snapshot(GameInputQueue::now());
        }

        for (auto& thread: threads)
            thread.join();

        queue.releaseAll(GameInputQueue::now());
        const auto& released = queue.snapshot(GameInputQueue::now());

        if (released.isDown(KeyCode::W) || released.isMouseDown(MouseButton::Left))
            ++stuckRounds;
    }

    check(stuckRounds == 0);

    queue.keyChanged(KeyCode::W, true, GameInputQueue::now());
    queue.mouseButtonChanged(MouseButton::Left, true, GameInputQueue::now());
    const auto& pressed = queue.snapshot(GameInputQueue::now());

    check(pressed.isDown(KeyCode::W));
    check(pressed.wasPressed(KeyCode::W));
    check(pressed.isMouseDown(MouseButton::Left));
    check(pressed.wasMousePressed(MouseButton::Left));
};

auto tHidTable = test("GameInput/hidUsagesMapToKeyCodes") = []
{
    check(keyCodeFromHidUsage(0x04) == KeyCode::A);
    check(keyCodeFromHidUsage(0x1A) == KeyCode::W);
    check(keyCodeFromHidUsage(0x27) == KeyCode::Num0);
    check(keyCodeFromHidUsage(0x29) == KeyCode::Escape);
    check(keyCodeFromHidUsage(0x2C) == KeyCode::Space);
    check(keyCodeFromHidUsage(0x50) == KeyCode::LeftArrow);
    check(keyCodeFromHidUsage(0x52) == KeyCode::UpArrow);
    check(keyCodeFromHidUsage(0xE1) == KeyCode::Shift);
    check(keyCodeFromHidUsage(0xE7) == KeyCode::RightCommand);
    check(keyCodeFromHidUsage(0x00) == KeyCode::Unknown);
    check(keyCodeFromHidUsage(0x7F) == KeyCode::Unknown);
};

auto tWindowFeed = test("GameInput/theWindowFeedReachesTheSnapshot") = []
{
    auto window = Window {};
    auto input = GameInput {window, GameInputSource::WindowEvents};

    check(input.backendName() == "Window events");

    window.events.input.activationChanged(true);
    window.events.input.keyEvent(keyEvent(KeyCode::W, KeyEventType::Down));
    window.events.input.keyEvent(keyEvent(KeyCode::W, KeyEventType::Down, true));
    window.events.input.mouseEvent(mouseEvent(MouseEventType::Moved, {4.0f, 1.0f}));
    window.events.input.mouseEvent(mouseEvent(MouseEventType::Down));

    const auto& frame = input.snapshot();

    check(frame.isDown(KeyCode::W));
    check(frame.wasPressed(KeyCode::W));
    check(frame.mouseDelta().x == 4.0f);
    check(frame.isMouseDown(MouseButton::Left));
    check(frame.events().size() == 3);
    check(frame.time() >= frame.newestEventTime());
};

auto tFocusLossReleases = test("GameInput/losingFocusReleasesEverything") = []
{
    auto window = Window {};
    auto input = GameInput {window, GameInputSource::WindowEvents};

    window.events.input.activationChanged(true);
    window.events.input.keyEvent(keyEvent(KeyCode::S, KeyEventType::Down));
    window.events.input.mouseEvent(mouseEvent(MouseEventType::Down));
    check(input.snapshot().isDown(KeyCode::S));

    window.events.input.activationChanged(false);
    const auto& frame = input.snapshot();

    check(!frame.isDown(KeyCode::S));
    check(frame.wasReleased(KeyCode::S));
    check(!frame.isMouseDown(MouseButton::Left));
};

auto tRepeatAfterRefocus =
    test("GameInput/aKeyStillHeldAfterRefocusReturnsOnItsRepeat") = []
{
    auto window = Window {};
    auto input = GameInput {window, GameInputSource::WindowEvents};

    window.events.input.activationChanged(true);
    window.events.input.keyEvent(keyEvent(KeyCode::W, KeyEventType::Down));
    check(input.snapshot().isDown(KeyCode::W));

    window.events.input.activationChanged(false);
    check(!input.snapshot().isDown(KeyCode::W));

    window.events.input.activationChanged(true);
    window.events.input.keyEvent(keyEvent(KeyCode::W, KeyEventType::Down, true));
    const auto& frame = input.snapshot();

    check(frame.isDown(KeyCode::W));
    check(frame.wasPressed(KeyCode::W));
};

auto tRepeatOfAHeldKey = test("GameInput/aRepeatOfAHeldKeyIsNoNewPress") = []
{
    auto window = Window {};
    auto input = GameInput {window, GameInputSource::WindowEvents};

    window.events.input.activationChanged(true);
    window.events.input.keyEvent(keyEvent(KeyCode::W, KeyEventType::Down));
    check(input.snapshot().wasPressed(KeyCode::W));

    window.events.input.keyEvent(keyEvent(KeyCode::W, KeyEventType::Down, true));
    window.events.input.keyEvent(keyEvent(KeyCode::W, KeyEventType::Down, true));
    const auto& frame = input.snapshot();

    check(frame.isDown(KeyCode::W));
    check(!frame.wasPressed(KeyCode::W));
    check(frame.events().empty());

    window.events.input.keyEvent(
        keyEvent(KeyCode::Unknown, KeyEventType::Down, true));
    check(input.snapshot().events().empty());
};

auto tPlatformFeedComesAndGoes = test("GameInput/thePlatformFeedComesAndGoes") = []
{
    auto window = Window {};

    for (auto round = 0; round < 3; ++round)
    {
        auto input = GameInput {window};
        check(!input.backendName().empty());
        check(input.snapshot().events().empty());
    }
};

auto tListenerRemovedOnDestruction =
    test("GameInput/destroyingItStopsListening") = []
{
    auto window = Window {};

    {
        auto input = GameInput {window, GameInputSource::WindowEvents};
    }

    window.events.input.keyEvent(keyEvent(KeyCode::W, KeyEventType::Down));
    window.events.input.activationChanged(false);
    check(!window.events.input.isActive());
};

namespace
{
bool windowKeyReachesSnapshot(Window& window, GameInput& input, uint16_t key)
{
    window.events.input.activationChanged(true);
    window.events.input.keyEvent(keyEvent(key, KeyEventType::Down));
    const auto pressed = input.snapshot().isDown(key);

    window.events.input.keyEvent(keyEvent(key, KeyEventType::Up));
    const auto released = input.snapshot().wasReleased(key);

    window.events.input.activationChanged(false);
    input.snapshot();

    return pressed && released;
}
} // namespace

auto tTwoPlatformFeedsCoexist =
    test("GameInput/twoPlatformFeedsOutliveEachOther") = []
{
    auto firstWindow = Window {};
    auto secondWindow = Window {};

    auto first = std::make_unique<GameInput>(firstWindow);
    auto second = std::make_unique<GameInput>(secondWindow);

    const auto name = std::string {second->backendName()};
    check(!first->backendName().empty());
    check(!name.empty());

    first.reset();

    check(second->backendName() == name);
    check(second->snapshot().events().empty());
    check(windowKeyReachesSnapshot(secondWindow, *second, KeyCode::A));

    second.reset();

    auto third = GameInput {firstWindow};
    check(!third.backendName().empty());
    check(third.snapshot().events().empty());
    check(windowKeyReachesSnapshot(firstWindow, third, KeyCode::D));
};

auto tWindowKeysUntilPlatformDelivers =
    test("GameInput/windowKeysCountUntilThePlatformDeliversOne") = []
{
    auto window = Window {};
    auto input = GameInput {window};

    check(input.backendName() == "Window events"
          || input.backendName() == "window keys, GameController mouse");
    check(windowKeyReachesSnapshot(window, input, KeyCode::W));
};
