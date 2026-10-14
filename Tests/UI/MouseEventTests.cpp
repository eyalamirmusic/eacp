#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

// What a press means beyond where it landed: whether it asks for a context
// menu, and whether it finished a double-click.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

using eacp::Graphics::ModifierKeys;
using eacp::Graphics::MouseButton;

namespace MouseEventTests
{
namespace
{
eacp::Graphics::MouseEvent mouseAt(Point position, int clickCount = 1)
{
    auto event = eacp::Graphics::MouseEvent {};

    event.pos = position;
    event.downPos = position;
    event.clickCount = clickCount;

    return event;
}

UI::MouseEvent pressWith(MouseButton button, ModifierKeys modifiers = {})
{
    auto event = UI::MouseEvent {};
    event.button = button;
    event.modifiers = modifiers;
    return event;
}

struct Recorder : Component
{
    Recorder() { setInterceptsMouseClicks(true); }

    void mouseDown(const UI::MouseEvent& event) override
    {
        events.push_back("down" + std::to_string(event.clickCount));
    }

    void mouseUp(const UI::MouseEvent&) override { events.push_back("up"); }

    void mouseDoubleClick(const UI::MouseEvent& event) override
    {
        events.push_back("double" + std::to_string(event.clickCount));
    }

    Vector<std::string> events;
};

struct Harness
{
    Harness()
    {
        host.setBounds({0.f, 0.f, 100.f, 100.f});
        host.setRootComponent(root);

        root.setBounds({0.f, 0.f, 100.f, 100.f});
        root.addAndMakeVisible(target);
        target.setBounds({0.f, 0.f, 100.f, 100.f});
    }

    void click(int clickCount)
    {
        host.mouseDown(mouseAt({50.f, 50.f}, clickCount));
        host.mouseUp(mouseAt({50.f, 50.f}, clickCount));
    }

    ComponentHost host;
    Component root;
    Recorder target;
};

Vector<std::string> sequence(std::initializer_list<const char*> names)
{
    auto result = Vector<std::string> {};

    for (auto* name: names)
        result.emplace_back(name);

    return result;
}
} // namespace

auto tRightClickIsPopup = test("MouseEvent/rightClickIsAPopupMenuClick") = []
{
    check(pressWith(MouseButton::Right).isPopupMenu());
    check(pressWith(MouseButton::Right, {.control = true}).isPopupMenu());
    check(!pressWith(MouseButton::Left).isPopupMenu());
    check(!pressWith(MouseButton::Middle).isPopupMenu());
};

// The Mac rule: a one-button mouse asks for a menu with Control. Elsewhere
// Control-click is an ordinary click with a modifier held.
auto tControlClickIsPopupOnMacOnly =
    test("MouseEvent/controlClickIsAPopupMenuClickOnMacOnly") = []
{
    auto controlClick = pressWith(MouseButton::Left, {.control = true});

    check(controlClick.isPopupMenu() == Platform::isMac());
    check(!pressWith(MouseButton::Left, {.command = true}).isPopupMenu());
    check(!pressWith(MouseButton::Middle, {.control = true}).isPopupMenu());
};

auto tHostEventAgrees = test("MouseEvent/hostAndComponentEventsAgree") = []
{
    auto hostEvent = eacp::Graphics::MouseEvent {};
    hostEvent.button = MouseButton::Left;
    hostEvent.modifiers.control = true;

    check(hostEvent.isPopupMenu()
          == pressWith(MouseButton::Left, {.control = true}).isPopupMenu());

    hostEvent.button = MouseButton::Right;
    hostEvent.modifiers = {};
    check(hostEvent.isPopupMenu());
};

auto tSingleClickNoDouble = test("MouseEvent/aSingleClickSendsNoDoubleClick") = []
{
    auto harness = Harness {};

    harness.click(1);

    check(harness.target.events == sequence({"down1", "up"}));
};

// The press and release arrive exactly as before, the down still carrying its
// count, so a component reading clickCount in mouseDown is not disturbed.
auto tDoubleClickAfterRelease =
    test("MouseEvent/aDoubleClickFollowsTheSecondRelease") = []
{
    auto harness = Harness {};

    harness.click(1);
    harness.click(2);

    check(harness.target.events
          == sequence({"down1", "up", "down2", "up", "double2"}));
};

// A platform may report 1 on the release of a double-click; the press decides.
auto tDoubleClickGoesByThePress =
    test("MouseEvent/theDoubleClickGoesByThePressCount") = []
{
    auto harness = Harness {};

    harness.host.mouseDown(mouseAt({50.f, 50.f}, 2));
    harness.host.mouseUp(mouseAt({50.f, 50.f}, 1));

    check(harness.target.events == sequence({"down2", "up", "double2"}));
};

auto tTripleClickNoDouble = test("MouseEvent/aTripleClickIsNotADoubleClick") = []
{
    auto harness = Harness {};

    harness.click(3);

    check(harness.target.events == sequence({"down3", "up"}));
};

auto tDoubleClickSkipsDeleted =
    test("MouseEvent/noDoubleClickForAComponentTheReleaseDestroyed") = []
{
    auto host = ComponentHost {};
    auto root = Component {};
    auto doubles = 0;

    host.setBounds({0.f, 0.f, 100.f, 100.f});
    host.setRootComponent(root);
    root.setBounds({0.f, 0.f, 100.f, 100.f});

    struct DeletesOnRelease : Component
    {
        explicit DeletesOnRelease(int& doublesToUse)
            : doubles(doublesToUse)
        {
            setInterceptsMouseClicks(true);
        }

        void mouseUp(const UI::MouseEvent&) override { owner->reset(); }
        void mouseDoubleClick(const UI::MouseEvent&) override { ++doubles; }

        int& doubles;
        std::unique_ptr<DeletesOnRelease>* owner = nullptr;
    };

    auto target = std::make_unique<DeletesOnRelease>(doubles);
    target->owner = &target;

    root.addAndMakeVisible(*target);
    target->setBounds({0.f, 0.f, 100.f, 100.f});

    host.mouseDown(mouseAt({50.f, 50.f}, 2));
    host.mouseUp(mouseAt({50.f, 50.f}, 2));

    check(target == nullptr);
    check(doubles == 0);
};
} // namespace MouseEventTests
