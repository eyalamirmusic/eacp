#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include "GameInputBackend.h"
#include "HidKeyCodes.h"

namespace eacp::Graphics
{
namespace
{
// What the handler blocks capture, by shared_ptr, so it outlives every block
// GameController still holds. `queue` is cleared on the handler queue itself
// during teardown, so a handler running after that sees null and pushes
// nothing; `ownerAlive` is the main thread's, for the connection
// notifications.
struct GameControllerShared
{
    GameInputQueue* queue = nullptr;
    const std::atomic<bool>* active = nullptr;
    std::atomic<bool> mouseDelivering {false};
    bool ownerAlive = true;

    bool accepting() const
    {
        return queue != nullptr && active->load(std::memory_order_relaxed);
    }
};

using SharedState = std::shared_ptr<GameControllerShared>;

dispatch_queue_t makeHandlerQueue()
{
    auto attributes = dispatch_queue_attr_make_with_qos_class(
        DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);

    return dispatch_queue_create("com.eacp.gameinput", attributes);
}

GCControllerButtonValueChangedHandler buttonHandler(const SharedState& state,
                                                    MouseButton button)
{
    auto shared = state;

    auto handler = ^(GCControllerButtonInput*, float, BOOL pressed)
    {
        if (!shared->accepting())
            return;

        shared->mouseDelivering.store(true);
        shared->queue->mouseButtonChanged(
            button, pressed == YES, GameInputQueue::now());
    };

    return [[handler copy] autorelease];
}

struct GameControllerBackend final : GameInputBackend
{
    GameControllerBackend(GameInputQueue& queue, const std::atomic<bool>& active)
    {
        state->queue = &queue;
        state->active = &active;

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

    ~GameControllerBackend() override
    {
        state->ownerAlive = false;

        for (id token in observers)
            [NSNotificationCenter.defaultCenter removeObserver:token];

        [observers release];

        detachKeyboard();

        NSArray<GCMouse*>* attached = [[mice copy] autorelease];

        for (GCMouse* mouse in attached)
            detachMouse(mouse);

        [mice release];

        auto shared = state;
        auto drain = ^{ shared->queue = nullptr; };
        dispatch_sync(handlerQueue, drain);
        dispatch_release(handlerQueue);
    }

    bool ownsKeys() const override { return keyboard != nil; }

    bool ownsMouse() const override
    {
        return mice.count > 0 && state->mouseDelivering.load();
    }

    using NotificationHandler = void (^)(NSNotification*);

    void observe(NSNotificationName name, NotificationHandler handler)
    {
        auto shared = state;

        auto guarded = ^(NSNotification* note)
        {
            if (shared->ownerAlive)
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
    }

    void attachKeyboard(GCKeyboard* candidate)
    {
        if (candidate == nil || candidate == keyboard)
            return;

        detachKeyboard();

        keyboard = [candidate retain];
        keyboard.handlerQueue = handlerQueue;

        auto shared = state;

        auto handler = ^(GCKeyboardInput*, GCDeviceButtonInput*, GCKeyCode code, BOOL pressed)
        {
            if (!shared->accepting())
                return;

            shared->queue->keyChanged(keyCodeFromHidUsage((uint32_t) code),
                                      pressed == YES,
                                      GameInputQueue::now());
        };

        keyboard.keyboardInput.keyChangedHandler = handler;
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

        auto shared = state;

        auto moved = ^(GCMouseInput*, float deltaX, float deltaY)
        {
            if (!shared->accepting())
                return;

            shared->mouseDelivering.store(true);
            shared->queue->mouseMoved({deltaX, -deltaY}, GameInputQueue::now());
        };

        auto* input = mouse.mouseInput;
        input.mouseMovedHandler = moved;
        input.leftButton.pressedChangedHandler =
            buttonHandler(state, MouseButton::Left);
        input.rightButton.pressedChangedHandler =
            buttonHandler(state, MouseButton::Right);
        input.middleButton.pressedChangedHandler =
            buttonHandler(state, MouseButton::Middle);
        input.auxiliaryButtons.firstObject.pressedChangedHandler =
            buttonHandler(state, MouseButton::Other);
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
            state->mouseDelivering.store(false);
    }

    SharedState state = std::make_shared<GameControllerShared>();
    dispatch_queue_t handlerQueue = makeHandlerQueue();
    NSMutableArray* observers = [[NSMutableArray alloc] init];
    NSMutableArray<GCMouse*>* mice = [[NSMutableArray alloc] init];
    GCKeyboard* keyboard = nil;
};
} // namespace

std::unique_ptr<GameInputBackend>
    makeGameInputBackend(GameInputQueue& queue, const std::atomic<bool>& active)
{
    return std::make_unique<GameControllerBackend>(queue, active);
}
} // namespace eacp::Graphics
