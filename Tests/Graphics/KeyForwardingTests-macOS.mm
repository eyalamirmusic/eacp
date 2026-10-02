#import <AppKit/AppKit.h>

#include "Common.h"

#include <eacp/Core/ObjC/ObjC.h>

#include <optional>

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
struct KeyLog
{
    struct Entry
    {
        bool isDown = false;
        uint16_t keyCode = 0;
    };

    void add(NSEvent* event)
    {
        entries.add({event.type == NSEventTypeKeyDown, event.keyCode});
    }

    int count(bool isDown, uint16_t keyCode) const
    {
        return entries.countIf([&](const Entry& entry)
                               { return entry.isDown == isDown && entry.keyCode == keyCode; });
    }

    bool receivedOnePress(uint16_t keyCode) const
    {
        return count(true, keyCode) == 1 && count(false, keyCode) == 1;
    }

    Vector<Entry> entries;
};

NSEvent* reencoded(NSEvent* event)
{
    return [NSEvent keyEventWithType:event.type
                            location:event.locationInWindow
                       modifierFlags:event.modifierFlags
                           timestamp:event.timestamp
                        windowNumber:event.windowNumber
                             context:nil
                          characters:event.characters
         charactersIgnoringModifiers:event.charactersIgnoringModifiers
                           isARepeat:NO
                             keyCode:event.keyCode];
}
} // namespace

@interface FakeDawPanel : NSView
@property(nonatomic, assign) KeyLog* log;
@property(nonatomic, assign) int bouncesLeft;
@end

@implementation FakeDawPanel
- (void) keyDown:(NSEvent*) event
{
    [self receive:event];
}

- (void) keyUp:(NSEvent*) event
{
    [self receive:event];
}

- (void) receive:(NSEvent*) event
{
    self.log->add(event);

    if (self.bouncesLeft <= 0)
        return;

    self.bouncesLeft = self.bouncesLeft - 1;
    [NSApp sendEvent:reencoded(event)];
}
@end

@interface FakePluginEditor : NSView
@property(nonatomic, assign) KeyLog* log;
@property(nonatomic, assign) BOOL passesUp;
@end

@implementation FakePluginEditor
- (BOOL) acceptsFirstResponder
{
    return YES;
}

- (void) keyDown:(NSEvent*) event
{
    self.log->add(event);

    if (self.passesUp)
        [super keyDown:event];
}

- (void) keyUp:(NSEvent*) event
{
    self.log->add(event);

    if (self.passesUp)
        [super keyUp:event];
}
@end

@interface RecordingTextView : NSTextView
@property(nonatomic, assign) KeyLog* log;
@end

@implementation RecordingTextView
- (void) keyDown:(NSEvent*) event
{
    self.log->add(event);
    [super keyDown:event];
}

- (void) keyUp:(NSEvent*) event
{
    self.log->add(event);
    [super keyUp:event];
}
@end

@interface FakeDawMainWindow : NSWindow
@property(nonatomic, assign) KeyLog* log;
@property(nonatomic, assign) int keyRequests;
@property(nonatomic, assign) int misaddressed;
@end

@implementation FakeDawMainWindow
- (void) sendEvent:(NSEvent*) event
{
    if (event.type != NSEventTypeKeyDown && event.type != NSEventTypeKeyUp)
    {
        [super sendEvent:event];
        return;
    }

    if (event.windowNumber != self.windowNumber)
        self.misaddressed = self.misaddressed + 1;

    self.log->add(event);
}

- (void) makeKeyWindow
{
    self.keyRequests = self.keyRequests + 1;
    [super makeKeyWindow];
}
@end

@interface KeyRequestCountingWindow : NSWindow
@property(nonatomic, assign) int keyRequests;
@end

@implementation KeyRequestCountingWindow
- (void) makeKeyWindow
{
    self.keyRequests = self.keyRequests + 1;
    [super makeKeyWindow];
}
@end

namespace
{
enum class Plugin
{
    PassesUp,
    SwallowsAll
};

enum class Repeat
{
    No,
    Yes
};

struct RecordingSurface : NativeChildSurface
{
    void keyDown(const KeyEvent& event) override { received.add(event); }
    void keyUp(const KeyEvent& event) override { received.add(event); }

    Vector<KeyEvent> received;
};

double nextTimestamp()
{
    static auto timestamp = [NSProcessInfo processInfo].systemUptime;
    timestamp += 1.0;
    return timestamp;
}

bool isSpace(const KeyEvent& key)
{
    return key.keyCode == KeyCode::Space;
}

WindowOptions pluginWindowOptions()
{
    auto options = WindowOptions {};
    options.width = 300;
    options.height = 200;
    options.showInactive = true;
    return options;
}

void showWithoutActivating(Window& window)
{
    auto* nsWindow = (NSWindow*) window.getHandle();
    nsWindow.alphaValue = 0.f;
    nsWindow.ignoresMouseEvents = YES;
    [nsWindow orderFrontRegardless];

    // An inactive app's window never becomes key, but asking is what makes
    // [NSApp sendEvent:] route key events to it.
    [nsWindow makeKeyWindow];
}

template <typename WindowClass>
struct PlainWindow
{
    PlainWindow(NSRect frame)
        : window([[WindowClass alloc]
              initWithContentRect:frame
                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable
                          backing:NSBackingStoreBuffered
                            defer:NO])
    {
        get().releasedWhenClosed = NO;
        get().alphaValue = 0.f;
        get().ignoresMouseEvents = YES;
        [get() orderFrontRegardless];
    }

    ~PlainWindow() { [get() orderOut:nil]; }

    WindowClass* get() { return window.get(); }

    ObjC::Ptr<WindowClass> window;
};

NSEvent* keyEvent(NSWindow* nsWindow, NSEventType type, uint16_t keyCode, Repeat repeat)
{
    auto* characters = keyCode == KeyCode::Space ? @" " : @"a";

    return [NSEvent keyEventWithType:type
                            location:NSZeroPoint
                       modifierFlags:0
                           timestamp:nextTimestamp()
                        windowNumber:nsWindow.windowNumber
                             context:nil
                          characters:characters
         charactersIgnoringModifiers:characters
                           isARepeat:repeat == Repeat::Yes
                             keyCode:keyCode];
}

NSEvent* keyEvent(Window& window, NSEventType type, uint16_t keyCode, Repeat repeat)
{
    return keyEvent((NSWindow*) window.getHandle(), type, keyCode, repeat);
}

NativeKeyEvent nativeKey(NSWindow* window, NSEventType type, uint16_t keyCode)
{
    auto* event = keyEvent(window, type, keyCode, Repeat::No);
    auto keyType = type == NSEventTypeKeyDown ? KeyEventType::Down : KeyEventType::Up;

    return {.key = {.keyCode = keyCode, .type = keyType, .timestamp = event.timestamp},
            .nativeKey = keyCode,
            .nsEvent = event};
}

void keyDown(Window& window, uint16_t keyCode, Repeat repeat = Repeat::No)
{
    [NSApp sendEvent:keyEvent(window, NSEventTypeKeyDown, keyCode, repeat)];
}

void keyUp(Window& window, uint16_t keyCode)
{
    [NSApp sendEvent:keyEvent(window, NSEventTypeKeyUp, keyCode, Repeat::No)];
}

void press(Window& window, uint16_t keyCode)
{
    keyDown(window, keyCode);
    keyUp(window, keyCode);
}

struct Topology
{
    explicit Topology(Plugin kind)
    {
        panel.get().log = &dawLog;
        embedded.setContentView(gesturesEditor);

        pluginContent.addSubview(surface);
        surface.setBounds({0.f, 0.f, 300.f, 200.f});

        editor.get().log = &pluginLog;
        editor.get().passesUp = kind == Plugin::PassesUp;
        [container() addSubview:editor.get()];

        showWithoutActivating(pluginWindow);
        focus(editor.get());
    }

    NSView* container() { return (NSView*) surface.getNativeParentHandle(); }

    void focus(NSView* view)
    {
        [(NSWindow*) pluginWindow.getHandle() makeFirstResponder:view];
    }

    void forwardUnhandledKeys()
    {
        surface.onUnhandledKey = [this](const NativeKeyEvent& key)
        {
            forwarder.forward(key);
            return true;
        };
    }

    void grabSpace()
    {
        grab.emplace(pluginWindow,
                     isSpace,
                     [this](const NativeKeyEvent& key) { forwarder.forward(key); });
    }

    void wireLikeGestures()
    {
        forwardUnhandledKeys();
        grabSpace();
    }

    void hostPanelInAWindow()
    {
        panelWindow.emplace(NSMakeRect(0, 0, 400, 300));
        panelWindow->get().contentView = panel.get();
    }

    FakeDawMainWindow* addDawMainWindow(NSRect frame = NSMakeRect(0, 0, 900, 700))
    {
        mainWindow.emplace(frame);
        mainWindow->get().log = &mainLog;
        return mainWindow->get();
    }

    void forwardPress(NSWindow* source, uint16_t keyCode)
    {
        forwarder.forward(nativeKey(source, NSEventTypeKeyDown, keyCode));
        forwarder.forward(nativeKey(source, NSEventTypeKeyUp, keyCode));
    }

    KeyLog dawLog;
    KeyLog mainLog;
    KeyLog pluginLog;
    ObjC::Ptr<FakeDawPanel> panel {
        [[FakeDawPanel alloc] initWithFrame:NSMakeRect(0, 0, 400, 300)]};
    View gesturesEditor;
    EmbeddedView embedded {(void*) panel.get()};
    EmbedderKeyForwarder forwarder {gesturesEditor};

    View pluginContent;
    RecordingSurface surface;
    Window pluginWindow {pluginContent, pluginWindowOptions()};
    ObjC::Ptr<FakePluginEditor> editor {
        [[FakePluginEditor alloc] initWithFrame:NSMakeRect(0, 0, 300, 200)]};
    std::optional<KeyGrab> grab;

    std::optional<PlainWindow<NSWindow>> panelWindow;
    std::optional<PlainWindow<FakeDawMainWindow>> mainWindow;
};
} // namespace

auto tUnusedKeyReachesTheDaw =
    test("HostedKeyForwarding/aKeyThePluginPassesUpReachesTheDaw") = []
{
    auto topology = Topology {Plugin::PassesUp};
    topology.wireLikeGestures();

    press(topology.pluginWindow, KeyCode::A);

    check(topology.pluginLog.receivedOnePress(KeyCode::A));
    check(topology.dawLog.receivedOnePress(KeyCode::A));
};

auto tGrabbedSpaceSkipsThePlugin =
    test("HostedKeyForwarding/grabbedSpaceGoesToTheDawAndNotThePlugin") = []
{
    auto topology = Topology {Plugin::SwallowsAll};
    topology.wireLikeGestures();

    press(topology.pluginWindow, KeyCode::Space);

    check(topology.dawLog.receivedOnePress(KeyCode::Space));
    check(topology.pluginLog.entries.empty());
};

auto tUngrabbedKeyStaysWithThePlugin =
    test("HostedKeyForwarding/ungrabbedKeyStaysWithASwallowingPlugin") = []
{
    auto topology = Topology {Plugin::SwallowsAll};
    topology.wireLikeGestures();

    press(topology.pluginWindow, KeyCode::A);

    check(topology.pluginLog.receivedOnePress(KeyCode::A));
    check(topology.dawLog.entries.empty());
};

auto tTextInputKeepsSpace =
    test("HostedKeyForwarding/focusedTextInputKeepsTheGrabbedKey") = []
{
    auto topology = Topology {Plugin::SwallowsAll};
    topology.wireLikeGestures();

    auto textLog = KeyLog {};
    auto textView = ObjC::Ptr<RecordingTextView> {
        [[RecordingTextView alloc] initWithFrame:NSMakeRect(0, 0, 100, 20)]};
    textView.get().log = &textLog;
    [topology.container() addSubview:textView.get()];
    topology.focus(textView.get());

    press(topology.pluginWindow, KeyCode::Space);

    check(textLog.count(true, KeyCode::Space) == 1);
    check(topology.dawLog.entries.empty());
    check(topology.pluginLog.entries.empty());
};

auto tDawEchoIsForwardedOnce =
    test("HostedKeyForwarding/aDawBouncingTheKeyBackGetsItOnce") = []
{
    auto topology = Topology {Plugin::PassesUp};
    topology.wireLikeGestures();
    topology.panel.get().bouncesLeft = 8;

    press(topology.pluginWindow, KeyCode::A);
    press(topology.pluginWindow, KeyCode::Space);

    check(topology.dawLog.receivedOnePress(KeyCode::A));
    check(topology.dawLog.receivedOnePress(KeyCode::Space));
    check(topology.pluginLog.receivedOnePress(KeyCode::A));
    check(topology.pluginLog.count(true, KeyCode::Space) == 0);
    check(topology.pluginLog.count(false, KeyCode::Space) == 0);
};

auto tEchoesAreSharedAcrossForwarders =
    test("HostedKeyForwarding/anEchoIsRecognisedByEveryForwarder") = []
{
    auto topology = Topology {Plugin::PassesUp};
    auto secondEditor = View {};
    auto secondEmbedded = EmbeddedView {(void*) topology.panel.get()};
    secondEmbedded.setContentView(secondEditor);
    auto secondForwarder = EmbedderKeyForwarder {secondEditor};

    auto* event = keyEvent(topology.pluginWindow, NSEventTypeKeyDown, KeyCode::B, Repeat::No);
    auto native = NativeKeyEvent {.key = {.keyCode = KeyCode::B,
                                          .type = KeyEventType::Down,
                                          .timestamp = event.timestamp},
                                  .nativeKey = KeyCode::B,
                                  .nsEvent = event};

    topology.forwarder.forward(native);
    secondForwarder.forwardUnpaired(native, [](const KeyEvent&) { return false; });

    check(topology.dawLog.count(true, KeyCode::B) == 1);
};

auto tGrabbedRepeatsAllReachTheDaw =
    test("HostedKeyForwarding/everyRepeatOfAGrabbedKeyReachesTheDaw") = []
{
    auto topology = Topology {Plugin::SwallowsAll};
    topology.wireLikeGestures();

    keyDown(topology.pluginWindow, KeyCode::Space);
    keyDown(topology.pluginWindow, KeyCode::Space, Repeat::Yes);
    keyDown(topology.pluginWindow, KeyCode::Space, Repeat::Yes);
    keyUp(topology.pluginWindow, KeyCode::Space);

    check(topology.dawLog.count(true, KeyCode::Space) == 3);
    check(topology.dawLog.count(false, KeyCode::Space) == 1);
    check(topology.pluginLog.entries.empty());
};

auto tGrabDestroyedWhileHeld =
    test("HostedKeyForwarding/destroyingTheGrabMidPressStillReleasesTheDaw") = []
{
    auto topology = Topology {Plugin::SwallowsAll};
    topology.wireLikeGestures();

    keyDown(topology.pluginWindow, KeyCode::Space);
    topology.grab.reset();
    keyUp(topology.pluginWindow, KeyCode::Space);

    check(topology.dawLog.receivedOnePress(KeyCode::Space));
    check(topology.pluginLog.count(true, KeyCode::Space) == 0);
};

auto tNoCallbackKeepsTheResponderChain =
    test("HostedKeyForwarding/withoutACallbackKeysGoWhereTheyAlwaysDid") = []
{
    auto topology = Topology {Plugin::PassesUp};

    press(topology.pluginWindow, KeyCode::A);

    check(topology.surface.received.size() == 2);
    check(topology.surface.received[0].keyCode == KeyCode::A);
    check(topology.surface.received[0].type == KeyEventType::Down);
    check(topology.surface.received[1].type == KeyEventType::Up);
    check(topology.dawLog.entries.empty());
};

auto tGrabIgnoresOtherWindows =
    test("HostedKeyForwarding/grabLeavesOtherWindowsAlone") = []
{
    auto topology = Topology {Plugin::SwallowsAll};
    topology.wireLikeGestures();

    auto otherLog = KeyLog {};
    auto otherContent = View {};
    auto other = Window {otherContent, pluginWindowOptions()};
    auto otherEditor = ObjC::Ptr<FakePluginEditor> {
        [[FakePluginEditor alloc] initWithFrame:NSMakeRect(0, 0, 50, 50)]};
    otherEditor.get().log = &otherLog;
    [(NSView*) otherContent.getHandle() addSubview:otherEditor.get()];
    showWithoutActivating(other);
    [(NSWindow*) other.getHandle() makeFirstResponder:otherEditor.get()];

    press(other, KeyCode::Space);

    check(otherLog.receivedOnePress(KeyCode::Space));
    check(topology.dawLog.entries.empty());
};

auto tGrabEndsWithItsObject =
    test("HostedKeyForwarding/destroyingTheGrabReleasesTheKey") = []
{
    auto topology = Topology {Plugin::SwallowsAll};
    topology.wireLikeGestures();
    topology.grab.reset();

    press(topology.pluginWindow, KeyCode::Space);

    check(topology.pluginLog.receivedOnePress(KeyCode::Space));
    check(topology.dawLog.entries.empty());
};

auto tSiblingKeysGoToTheHostMainWindow =
    test("HostedKeyForwarding/keysFromASiblingWindowGoToTheHostMainWindow") = []
{
    auto topology = Topology {Plugin::PassesUp};
    topology.hostPanelInAWindow();
    auto* main = topology.addDawMainWindow();
    topology.wireLikeGestures();

    press(topology.pluginWindow, KeyCode::A);
    press(topology.pluginWindow, KeyCode::Space);

    check(topology.mainLog.receivedOnePress(KeyCode::A));
    check(topology.mainLog.receivedOnePress(KeyCode::Space));
    check(topology.dawLog.entries.empty());
    check(main.misaddressed == 0);
    check(main.keyRequests == 4);
};

auto tKeyStatusIsHandedBack =
    test("HostedKeyForwarding/theSourceWindowIsMadeKeyAgainAfterDelivery") = []
{
    auto topology = Topology {Plugin::PassesUp};
    topology.hostPanelInAWindow();
    topology.addDawMainWindow();
    auto source = PlainWindow<KeyRequestCountingWindow> {NSMakeRect(0, 0, 200, 100)};

    topology.forwardPress(source.get(), KeyCode::A);

    check(topology.mainLog.receivedOnePress(KeyCode::A));
    check(NSApp.keyWindow != nil || source.get().keyRequests == 2);
};

auto tPanelKeysKeepTheResponderChain =
    test("HostedKeyForwarding/aKeyFromInsideThePanelWalksTheResponderChain") = []
{
    auto topology = Topology {Plugin::PassesUp};
    topology.hostPanelInAWindow();
    auto* main = topology.addDawMainWindow();

    topology.forwardPress(topology.panelWindow->get(), KeyCode::A);

    check(topology.dawLog.receivedOnePress(KeyCode::A));
    check(topology.mainLog.entries.empty());
    check(main.keyRequests == 0);
};

auto tNoHostMainWindowFallsBack =
    test("HostedKeyForwarding/withNoHostMainWindowSiblingKeysWalkTheResponderChain") = []
{
    auto topology = Topology {Plugin::PassesUp};
    topology.hostPanelInAWindow();
    topology.wireLikeGestures();

    press(topology.pluginWindow, KeyCode::A);

    check(topology.dawLog.receivedOnePress(KeyCode::A));
};

auto tFrameworkWindowsAreNeverTheHost =
    test("HostedKeyForwarding/anEacpWindowIsNeverTakenForTheHostMainWindow") = []
{
    auto topology = Topology {Plugin::PassesUp};
    topology.hostPanelInAWindow();
    topology.wireLikeGestures();

    auto bigContent = View {};
    auto bigOptions = pluginWindowOptions();
    bigOptions.width = 1400;
    bigOptions.height = 1000;
    auto big = Window {bigContent, bigOptions};
    showWithoutActivating(big);
    showWithoutActivating(topology.pluginWindow);

    press(topology.pluginWindow, KeyCode::A);
    check(topology.dawLog.receivedOnePress(KeyCode::A));

    topology.addDawMainWindow(NSMakeRect(0, 0, 300, 200));
    press(topology.pluginWindow, KeyCode::B);
    check(topology.mainLog.receivedOnePress(KeyCode::B));
    check(topology.dawLog.count(true, KeyCode::B) == 0);
};
