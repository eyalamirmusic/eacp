#include "Common.h"

// View::getWindow() - a view finding the window it is in, rather than being
// handed it.
//
// The three cases that matter are the three answers it can give: none yet, this
// one, and none any more. The last is the one worth having a test for at all: a
// back-pointer that survives the thing it points at is worse than no
// back-pointer, and the destructor that clears it is a member of Window rather
// than a line in each platform's `= default` destructor precisely so it cannot
// be forgotten on one of them.
//
// No window is ever shown here; setContentView is enough for the link, and the
// tests run headless like the rest of the Window suite.

using namespace nano;
using namespace eacp::Graphics;

auto tViewHasNoWindowUntilAdopted = test("View/hasNoWindowUntilAdopted") = []
{
    auto view = View {};

    check(view.getWindow() == nullptr);

    auto window = Window {};
    window.setContentView(view);

    check(view.getWindow() == &window);
};

// A subview is in its root's window: the pointer lives on the view the window
// adopted, and everything under it walks up. Reparenting therefore needs no
// bookkeeping - the answer follows the chain the subtree already moved along.
auto tSubviewFindsTheWindow = test("View/subviewFindsTheWindow") = []
{
    auto content = View {};
    auto child = View {};
    auto grandchild = View {};

    content.addSubview(child);
    child.addSubview(grandchild);

    check(grandchild.getWindow() == nullptr);

    auto window = Window {};
    window.setContentView(content);

    check(child.getWindow() == &window);
    check(grandchild.getWindow() == &window);

    // Taken out of the tree, it is in no window - without anything having told
    // it so.
    child.removeFromParent();

    check(child.getWindow() == nullptr);
    check(grandchild.getWindow() == nullptr);
    check(content.getWindow() == &window);
};

// The case a raw back-pointer gets wrong. The view outlives the window here,
// which is the ordinary shape of an app whose view is a member declared after
// its window.
auto tViewOutlivingItsWindowReportsNone = test("View/outlivingItsWindowIsSafe") = []
{
    auto view = View {};

    {
        auto window = Window {};
        window.setContentView(view);
        check(view.getWindow() == &window);
    }

    check(view.getWindow() == nullptr);
};

// Adopting a second view releases the first, so two windows and two views never
// leave a view pointing at a window that is not showing it.
auto tAdoptingASecondViewReleasesTheFirst =
    test("View/adoptingASecondViewReleasesTheFirst") = []
{
    auto first = View {};
    auto second = View {};
    auto window = Window {};

    window.setContentView(first);
    check(first.getWindow() == &window);

    window.setContentView(second);

    check(second.getWindow() == &window);
    check(first.getWindow() == nullptr);
};

// The window built with its view: `View view; Window window {view};` as two
// members, adopted with no constructor body to do it in.
auto tWindowAdoptsViewOnConstruction = test("Window/adoptsViewOnConstruction") = []
{
    auto view = View {};
    auto options = WindowOptions {};
    options.width = 500;
    options.height = 320;

    auto window = Window {view, options};
    auto size = window.getSize();

    check(view.getWindow() == &window);
    check(size.x == 500.f);
    check(size.y == 320.f);
};

namespace
{
struct NumberedView final : View
{
    explicit NumberedView(int numberToKeep)
        : number(numberToKeep)
    {
    }

    int number = 0;
};
} // namespace

// ViewWindow builds the view from the arguments after the options, then the
// window showing it, so the pair is one member and the app struct it used to
// be is not needed at all.
auto tViewWindowBuildsViewFromArgsThenWindow =
    test("ViewWindow/buildsViewFromArgsThenWindow") = []
{
    auto options = WindowOptions {};
    options.width = 500;
    options.height = 320;

    auto shown = ViewWindow<NumberedView> {options, 7};
    auto size = shown.window.getSize();

    check(shown.view.number == 7);
    check(shown.view.getWindow() == &shown.window);
    check(size.x == 500.f);
    check(size.y == 320.f);
};

auto tViewWindowDefaultConstructs = test("ViewWindow/defaultConstructs") = []
{
    auto shown = ViewWindow<View> {};

    check(shown.view.getWindow() == &shown.window);
};
