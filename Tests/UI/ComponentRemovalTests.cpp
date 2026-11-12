#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <optional>

// A subtree taken out of the tree with removeChildComponent, or by the host's
// root being replaced, takes the host's hover, focus, press and file-drag target
// with it, silently: the removed components are told nothing, the next event
// goes to whatever is in the tree, and nothing is ever routed to them again,
// whether they are destroyed afterwards or stay alive.
//
// "Destroyed" is a std::optional reset and emplaced again, so the new component
// sits where the old one was: a host still holding the old pointer routes to the
// new one, which records it, rather than writing to freed memory.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace ComponentRemovalTests
{
namespace
{
using Files = Vector<std::string>;

eacp::Graphics::MouseEvent mouseAt(Point position)
{
    auto event = eacp::Graphics::MouseEvent {};

    event.pos = position;
    event.downPos = position;

    return event;
}

struct Probe final
    : Component
    , FileDropTarget
{
    Probe()
    {
        setInterceptsMouseClicks(true);
        setWantsKeyboardFocus(true);
    }

    void mouseEnter(const MouseEvent&) override { events.push_back("enter"); }
    void mouseExit(const MouseEvent&) override { events.push_back("exit"); }
    void mouseMove(const MouseEvent&) override { events.push_back("move"); }
    void mouseDown(const MouseEvent&) override { events.push_back("down"); }
    void mouseDrag(const MouseEvent&) override { events.push_back("drag"); }
    void mouseUp(const MouseEvent&) override { events.push_back("up"); }
    void focusGained() override { events.push_back("focusGained"); }
    void focusLost() override { events.push_back("focusLost"); }

    bool keyDown(const KeyEvent&) override
    {
        events.push_back("key");
        return true;
    }

    bool isInterestedInFileDrop(const Files&) override { return true; }
    void fileDragEnter(const Files&, Point) override
    {
        events.push_back("fileEnter");
    }
    void fileDragMove(const Files&, Point) override { events.push_back("fileMove"); }
    void fileDragExit(const Files&) override { events.push_back("fileExit"); }
    void filesDropped(const Files&, Point) override { events.push_back("fileDrop"); }

    Vector<std::string> events;
};

const auto inside = Point {20.f, 20.f};
const auto elsewhere = Point {150.f, 150.f};

// The probe covers the top-left 50x50 of a 200x200 root, inside a panel of the
// same size, so the probe or its panel can be what is removed.
struct Harness
{
    Harness()
    {
        host.setBounds({0.f, 0.f, 200.f, 200.f});
        host.setRootComponent(root);
        root.setBounds({0.f, 0.f, 200.f, 200.f});

        root.addAndMakeVisible(*panel);
        panel->setBounds({0.f, 0.f, 50.f, 50.f});

        panel->addAndMakeVisible(*probe);
        probe->setBounds({0.f, 0.f, 50.f, 50.f});
    }

    // Hovered, focused and pressed, with the button still down.
    void pressProbe()
    {
        host.mouseMoved(mouseAt(inside));
        host.mouseDown(mouseAt(inside));

        check(probe->isMouseOver());
        check(probe->hasKeyboardFocus());
    }

    void destroyProbe()
    {
        probe.reset();
        probe.emplace();
    }

    // Every kind of event the host routes, none of them over the probe's spot.
    void sendEverything()
    {
        host.mouseMoved(mouseAt(elsewhere));
        host.mouseDragged(mouseAt(elsewhere));
        host.mouseUp(mouseAt(elsewhere));
        host.mouseMoved(mouseAt(elsewhere));
        host.mouseExited(mouseAt(elsewhere));
        host.keyDown(KeyEvent {});
        host.keyUp(KeyEvent {});
    }

    ComponentHost host;
    Component root;
    std::optional<Component> panel {std::in_place};
    std::optional<Probe> probe {std::in_place};
};
} // namespace

auto tRemovedHoverDestroyed =
    test("ComponentRemoval/aHoveredChildRemovedThenDestroyedIsNotExited") = []
{
    auto harness = Harness {};

    harness.host.mouseMoved(mouseAt(inside));
    check(harness.probe->isMouseOver());

    harness.panel->removeChildComponent(*harness.probe);
    check(!harness.probe->isMouseOver(), "removal ends the hover");

    harness.destroyProbe();
    harness.host.mouseMoved(mouseAt(elsewhere));
    harness.host.mouseExited(mouseAt(elsewhere));

    check(harness.probe->events.empty());
};

auto tRemovedFocusDestroyed =
    test("ComponentRemoval/aFocusedChildRemovedThenDestroyedGetsNoKeys") = []
{
    auto harness = Harness {};

    harness.probe->grabKeyboardFocus();
    check(harness.host.getFocusedComponent() == &*harness.probe);

    harness.panel->removeChildComponent(*harness.probe);
    check(harness.host.getFocusedComponent() == nullptr);

    harness.destroyProbe();
    harness.host.keyDown(KeyEvent {});

    check(harness.probe->events.empty());
};

auto tRemovedPressDestroyed = test(
    "ComponentRemoval/aPressedChildRemovedThenDestroyedGetsNoDragOrRelease") = []
{
    auto harness = Harness {};

    harness.host.mouseDown(mouseAt(inside));
    harness.panel->removeChildComponent(*harness.probe);
    harness.destroyProbe();

    harness.host.mouseDragged(mouseAt(elsewhere));
    harness.host.mouseUp(mouseAt(elsewhere));

    check(harness.probe->events.empty());
};

auto tRemovedFileTargetDestroyed =
    test("ComponentRemoval/aFileDragTargetRemovedThenDestroyedIsNotExited") = []
{
    auto harness = Harness {};
    auto files = Files {"/tmp/kick.wav"};

    check(harness.host.fileDragEnter(files, inside));

    harness.panel->removeChildComponent(*harness.probe);
    harness.destroyProbe();

    harness.host.fileDragMove(files, elsewhere);
    harness.host.fileDragExit();

    check(harness.probe->events.empty());
};

auto tRemovedAncestor =
    test("ComponentRemoval/removingAnAncestorForgetsEverythingInsideIt") = []
{
    auto harness = Harness {};
    auto files = Files {"/tmp/kick.wav"};

    harness.pressProbe();
    check(harness.host.fileDragEnter(files, inside));

    harness.root.removeChildComponent(*harness.panel);

    check(harness.host.getFocusedComponent() == nullptr);
    check(!harness.probe->isMouseOver());

    harness.destroyProbe();
    harness.panel.reset();

    harness.sendEverything();
    harness.host.fileDragMove(files, elsewhere);
    harness.host.fileDragExit();

    check(harness.probe->events.empty());
};

auto tRemovedButAlive =
    test("ComponentRemoval/aRemovedComponentThatStaysAliveIsRoutedNothing") = []
{
    auto harness = Harness {};

    harness.pressProbe();
    harness.probe->events.clear();

    harness.panel->removeChildComponent(*harness.probe);

    check(harness.host.getFocusedComponent() == nullptr);
    check(!harness.probe->hasKeyboardFocus());
    check(!harness.probe->isMouseOver());

    harness.sendEverything();

    check(harness.probe->events.empty(), "and none at removal either");

    // Put back, it is an ordinary component again.
    harness.panel->addAndMakeVisible(*harness.probe);
    harness.host.mouseMoved(mouseAt(inside));

    check(harness.probe->isMouseOver());
};

auto tMovedOutOfTheHost =
    test("ComponentRemoval/movingAChildToAParentOutsideTheHostForgetsIt") = []
{
    auto harness = Harness {};
    auto outside = Component {};

    harness.pressProbe();
    harness.probe->events.clear();

    outside.addAndMakeVisible(*harness.probe);

    check(harness.host.getFocusedComponent() == nullptr);
    check(!harness.probe->isMouseOver());

    harness.sendEverything();

    check(harness.probe->events.empty());
};

// A move from one parent to another inside the same tree is not a removal: the
// press goes on, and the focus and hover stay where they are.
auto tMovedWithinTheHost =
    test("ComponentRemoval/movingAChildWithinTheHostKeepsItsPressAndFocus") = []
{
    auto harness = Harness {};
    auto other = Component {};

    harness.root.addAndMakeVisible(other);
    other.setBounds({0.f, 0.f, 50.f, 50.f});

    harness.pressProbe();
    harness.probe->events.clear();

    other.addAndMakeVisible(*harness.probe);

    check(harness.host.getFocusedComponent() == &*harness.probe);
    check(harness.probe->isMouseOver());

    harness.host.mouseDragged(mouseAt(inside));
    harness.host.mouseUp(mouseAt(inside));

    check(harness.probe->events == Vector<std::string> {"drag", "up"});
};

auto tReplacedRoot =
    test("ComponentRemoval/replacingTheRootForgetsEverythingInTheOldTree") = []
{
    auto harness = Harness {};
    auto files = Files {"/tmp/kick.wav"};
    auto newRoot = Component {};

    harness.pressProbe();
    check(harness.host.fileDragEnter(files, inside));

    harness.host.setRootComponent(newRoot);

    check(harness.host.getFocusedComponent() == nullptr);
    check(!harness.probe->isMouseOver());

    harness.destroyProbe();

    harness.sendEverything();
    harness.host.fileDragMove(files, elsewhere);
    harness.host.fileDragExit();

    check(harness.probe->events.empty());
};
} // namespace ComponentRemovalTests
