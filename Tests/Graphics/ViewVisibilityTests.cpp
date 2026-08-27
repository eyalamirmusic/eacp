#include "Common.h"

// View::setVisible — the portable half.
//
// What this covers is the bookkeeping around the flag rather than the flag
// itself: who gets told, and who deliberately does not. The point of the
// notification is that a view hosting a separate platform surface (a WebView,
// whose page the OS otherwise keeps live and expensive) has to hear about a
// hide that happened somewhere above it, and must not hear about a "show" that
// does not actually put it back on screen.
//
// The platform half is ViewVisibilityTests-macOS.mm: whether AppKit was really
// told, and whether it carries the news down to a nested native view.

using namespace nano;
using namespace eacp::Graphics;

namespace
{
// Counts as well as remembers: a view told "hidden" twice has a bug that the
// final state alone cannot show.
struct RecordingView final : View
{
    void visibilityChanged(bool visible) override
    {
        ++notifications;
        lastVisible = visible;
    }

    int notifications = 0;
    bool lastVisible = true;
};
} // namespace

auto tViewStartsVisible = test("ViewVisibility/startsVisible") = []
{
    auto view = View {};

    check(view.isVisible());
};

auto tSetVisibleFlipsTheFlag = test("ViewVisibility/setVisibleFlipsTheFlag") = []
{
    auto view = View {};

    view.setVisible(false);
    check(!view.isVisible());

    view.setVisible(true);
    check(view.isVisible());
};

// The view being hidden hears about it first-hand.
auto tHiddenViewIsNotified = test("ViewVisibility/hiddenViewIsNotified") = []
{
    auto view = RecordingView {};

    view.setVisible(false);

    check(view.notifications == 1);
    check(view.lastVisible == false);
};

// Setting the value it already has is not a change. Without this a container
// re-running its layout would announce a hide on every pass, and a WebView
// would re-apply page visibility it never left.
auto tRedundantSetIsSilent = test("ViewVisibility/redundantSetIsSilent") = []
{
    auto view = RecordingView {};

    view.setVisible(true);
    check(view.notifications == 0);

    view.setVisible(false);
    view.setVisible(false);
    check(view.notifications == 1);
};

// The reason the notification is a subtree walk. The descendant's own flag never
// moved, but it is off screen now, and it is the one holding the native surface.
auto tSubtreeHearsAnAncestorHide =
    test("ViewVisibility/subtreeHearsAnAncestorHide") = []
{
    auto parent = View {};
    auto child = RecordingView {};
    auto grandchild = RecordingView {};

    parent.addSubview(child);
    child.addSubview(grandchild);

    parent.setVisible(false);

    check(child.notifications == 1);
    check(child.lastVisible == false);
    check(grandchild.notifications == 1);
    check(grandchild.lastVisible == false);

    // Its own flag is untouched: it is hidden by where it sits, not by its own
    // state, and it must come back when the parent does.
    check(child.isVisible());
};

// A branch hidden in its own right stays hidden when an ancestor is shown, so
// it is not told it is visible. Telling it otherwise is the bug this guards: a
// WebView would un-throttle a page that is still not on screen.
auto tOwnHideSurvivesAnAncestorShow =
    test("ViewVisibility/ownHideSurvivesAnAncestorShow") = []
{
    auto parent = View {};
    auto child = View {};
    auto grandchild = RecordingView {};

    parent.addSubview(child);
    child.addSubview(grandchild);

    child.setVisible(false);
    auto before = grandchild.notifications;

    parent.setVisible(false);
    parent.setVisible(true);

    check(grandchild.notifications == before);
    check(!child.isVisible());
};
