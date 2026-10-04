#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include "GameInputBackend.h"
#include "HidKeyCodes.h"
#include "../View/View.h"

#include <vector>

namespace eacp::Graphics
{
namespace
{
// One GameInput's end of the process-wide feed. `keysDelivering` and
// `mouseDelivering` are set on the handler queue when GameController first
// hands this GameInput an event, and read on the main thread.
struct GameInputSink
{
    GameInputQueue* queue = nullptr;
    const std::atomic<bool>* active = nullptr;
    std::atomic<bool> keysDelivering {false};
    std::atomic<bool> mouseDelivering {false};

    bool accepting() const { return active->load(std::memory_order_relaxed); }
};

using Sink = std::shared_ptr<GameInputSink>;

// What the handler blocks capture, by shared_ptr, so it outlives every block
// GameController still holds. `sinks` is touched only on the handler queue,
// so a handler running after the last sink left sees none and pushes nothing;
// `ownerAlive` is the main thread's, for the connection notifications.
struct HubShared
{
    std::vector<Sink> sinks;
    bool ownerAlive = true;

    template <typename Function>
    void forEachAccepting(Function&& function)
    {
        for (auto& sink: sinks)
            if (sink->accepting())
                function(*sink);
    }
};

using SharedState = std::shared_ptr<HubShared>;

dispatch_queue_t makeHandlerQueue()
{
    auto attributes = dispatch_queue_attr_make_with_qos_class(
        DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);

    return dispatch_queue_create("com.eacp.gameinput", attributes);
}

// Run on the first key GameController hands a sink: keys the window fed in
// before it took over are set to what GameController says is held, so none is
// left down without a release to come, or pressed a second time. The key
// whose event triggered this is skipped: that event is pushed next and is
// authoritative, where its button may not reflect it yet.
void reconcileHeldKeys(GameInputQueue& queue,
                       GCKeyboardInput* input,
                       uint16_t triggeringKey,
                       double time)
{
    constexpr auto usageCount = uint32_t {256};

    for (auto usage = uint32_t {0}; usage < usageCount; ++usage)
    {
        const auto key = keyCodeFromHidUsage(usage);

        if (key == KeyCode::Unknown || key == triggeringKey)
            continue;

        auto* button = [input buttonForKeyCode:(GCKeyCode) usage];
        queue.keyChanged(key, button != nil && button.isPressed, time);
    }
}

GCKeyboardValueChangedHandler keyHandler(const SharedState& state)
{
    auto shared = state;

    auto handler = ^(GCKeyboardInput* input,
                     GCControllerButtonInput*,
                     GCKeyCode code,
                     BOOL pressed)
    {
        const auto key = keyCodeFromHidUsage((uint32_t) code);
        const auto time = GameInputQueue::now();

        shared->forEachAccepting(
            [&](GameInputSink& sink)
            {
                if (!sink.keysDelivering.exchange(true))
                    reconcileHeldKeys(*sink.queue, input, key, time);

                sink.queue->keyChanged(key, pressed == YES, time);
            });
    };

    return [[handler copy] autorelease];
}

GCMouseMoved mouseMovedHandler(const SharedState& state)
{
    auto shared = state;

    auto handler = ^(GCMouseInput*, float deltaX, float deltaY)
    {
        const auto time = GameInputQueue::now();

        shared->forEachAccepting(
            [&](GameInputSink& sink)
            {
                sink.mouseDelivering.store(true);
                sink.queue->mouseMoved({deltaX, -deltaY}, time);
            });
    };

    return [[handler copy] autorelease];
}

GCControllerButtonValueChangedHandler buttonHandler(const SharedState& state,
                                                    MouseButton button)
{
    auto shared = state;

    auto handler = ^(GCControllerButtonInput*, float, BOOL pressed)
    {
        const auto time = GameInputQueue::now();

        shared->forEachAccepting(
            [&](GameInputSink& sink)
            {
                sink.mouseDelivering.store(true);
                sink.queue->mouseButtonChanged(button, pressed == YES, time);
            });
    };

    return [[handler copy] autorelease];
}

// GCKeyboard.coalescedKeyboard and every GCMouse are process-wide, each with
// one set of handlers and one handler queue, so the handlers are installed
// once, by the first GameInput, and fan out to every GameInput's sink. The
// last one to leave takes them down. Main thread only, but for `shared`.
class GameControllerHub
{
public:
    static GameControllerHub& join(const Sink& sink)
    {
        if (instance == nullptr)
            instance = new GameControllerHub();

        instance->add(sink);
        return *instance;
    }

    static void leave(const Sink& sink)
    {
        if (instance == nullptr || !instance->remove(sink))
            return;

        delete instance;
        instance = nullptr;
    }

    bool hasMice() const { return mice.count > 0; }

private:
    GameControllerHub()
    {
        observe(GCKeyboardDidConnectNotification,
                ^(NSNotification* note) { keyboardConnected(note.object); });
        observe(GCKeyboardDidDisconnectNotification,
                ^(NSNotification* note) { keyboardDisconnected(note.object); });
        observe(GCMouseDidConnectNotification,
                ^(NSNotification* note) { attachMouse(note.object); });
        observe(GCMouseDidDisconnectNotification,
                ^(NSNotification* note) { detachMouse(note.object); });

        attachKeyboard(GCKeyboard.coalescedKeyboard);

        for (GCMouse* mouse in GCMouse.mice)
            attachMouse(mouse);
    }

    ~GameControllerHub()
    {
        shared->ownerAlive = false;

        for (id token in observers)
            [NSNotificationCenter.defaultCenter removeObserver:token];

        [observers release];

        detachKeyboard();

        NSArray<GCMouse*>* attached = [[mice copy] autorelease];

        for (GCMouse* mouse in attached)
            detachMouse(mouse);

        [mice release];

        auto state = shared;
        auto drain = ^{ state->sinks.clear(); };
        dispatch_sync(handlerQueue, drain);
        dispatch_release(handlerQueue);
    }

    void add(const Sink& sink)
    {
        auto state = shared;
        auto added = sink;
        auto addition = ^{ state->sinks.push_back(added); };
        dispatch_sync(handlerQueue, addition);

        ++users;
    }

    // Once this returns no handler is pushing into the sink's queue. True when
    // it was the last.
    bool remove(const Sink& sink)
    {
        auto state = shared;
        auto removed = sink;
        auto removal = ^{ std::erase(state->sinks, removed); };
        dispatch_sync(handlerQueue, removal);

        return --users == 0;
    }

    template <typename Function>
    void forEachSink(Function function)
    {
        auto state = shared;

        auto visit = ^{
            for (auto& sink: state->sinks)
                function(*sink);
        };

        dispatch_sync(handlerQueue, visit);
    }

    using NotificationHandler = void (^)(NSNotification*);

    void observe(NSNotificationName name, NotificationHandler handler)
    {
        auto state = shared;

        auto guarded = ^(NSNotification* note)
        {
            if (state->ownerAlive)
                handler(note);
        };

        auto* center = NSNotificationCenter.defaultCenter;
        id token = [center addObserverForName:name
                                       object:nil
                                        queue:NSOperationQueue.mainQueue
                                   usingBlock:guarded];

        [observers addObject:token];
    }

    void keyboardConnected(GCKeyboard* connected)
    {
        if (keyboard == nil)
            attachKeyboard(connected);
    }

    void keyboardDisconnected(GCKeyboard* disconnected)
    {
        if (disconnected != keyboard)
            return;

        detachKeyboard();
        attachKeyboard(GCKeyboard.coalescedKeyboard);

        if (keyboard == nil)
            forEachSink([](GameInputSink& sink)
                        { sink.keysDelivering.store(false); });
    }

    void attachKeyboard(GCKeyboard* candidate)
    {
        if (candidate == nil || candidate == keyboard)
            return;

        detachKeyboard();

        keyboard = [candidate retain];
        keyboard.handlerQueue = handlerQueue;
        keyboard.keyboardInput.keyChangedHandler = keyHandler(shared);
    }

    void detachKeyboard()
    {
        if (keyboard == nil)
            return;

        keyboard.keyboardInput.keyChangedHandler = nil;
        keyboard.handlerQueue = dispatch_get_main_queue();
        [keyboard release];
        keyboard = nil;
    }

    void attachMouse(GCMouse* mouse)
    {
        if (mouse == nil || [mice containsObject:mouse])
            return;

        [mice addObject:mouse];
        mouse.handlerQueue = handlerQueue;

        auto* input = mouse.mouseInput;
        input.mouseMovedHandler = mouseMovedHandler(shared);
        input.leftButton.pressedChangedHandler =
            buttonHandler(shared, MouseButton::Left);
        input.rightButton.pressedChangedHandler =
            buttonHandler(shared, MouseButton::Right);
        input.middleButton.pressedChangedHandler =
            buttonHandler(shared, MouseButton::Middle);
        input.auxiliaryButtons.firstObject.pressedChangedHandler =
            buttonHandler(shared, MouseButton::Other);
    }

    void detachMouse(GCMouse* mouse)
    {
        if (mouse == nil || ![mice containsObject:mouse])
            return;

        auto* input = mouse.mouseInput;
        input.mouseMovedHandler = nil;
        input.leftButton.pressedChangedHandler = nil;
        input.rightButton.pressedChangedHandler = nil;
        input.middleButton.pressedChangedHandler = nil;
        input.auxiliaryButtons.firstObject.pressedChangedHandler = nil;
        mouse.handlerQueue = dispatch_get_main_queue();

        [mice removeObject:mouse];

        if (mice.count == 0)
            forEachSink([](GameInputSink& sink)
                        { sink.mouseDelivering.store(false); });
    }

    static inline GameControllerHub* instance = nullptr;

    SharedState shared = std::make_shared<HubShared>();
    dispatch_queue_t handlerQueue = makeHandlerQueue();
    NSMutableArray* observers = [[NSMutableArray alloc] init];
    NSMutableArray<GCMouse*>* mice = [[NSMutableArray alloc] init];
    GCKeyboard* keyboard = nil;
    int users = 0;
};

struct GameControllerBackend final : GameInputBackend
{
    GameControllerBackend(GameInputQueue& queue, const std::atomic<bool>& active)
        : sink(makeSink(queue, active))
        , hub(GameControllerHub::join(sink))
    {
    }

    ~GameControllerBackend() override { GameControllerHub::leave(sink); }

    bool ownsKeys() const override { return sink->keysDelivering.load(); }

    bool ownsMouse() const override
    {
        return hub.hasMice() && sink->mouseDelivering.load();
    }

    static Sink makeSink(GameInputQueue& queue, const std::atomic<bool>& active)
    {
        auto made = std::make_shared<GameInputSink>();
        made->queue = &queue;
        made->active = &active;
        return made;
    }

    Sink sink;
    GameControllerHub& hub;
};
} // namespace

std::unique_ptr<GameInputBackend>
    makeGameInputBackend(GameInputQueue& queue, const std::atomic<bool>& active)
{
    return std::make_unique<GameControllerBackend>(queue, active);
}
} // namespace eacp::Graphics
