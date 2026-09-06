#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

// Tabs, which are one index and the visibility that follows it.
//
// The bar is drawn by one component rather than built out of latching buttons,
// so the thing worth pinning is that its index and the pages never disagree:
// exactly one page is visible, it is the current one, and it is the only one
// laid out. A page that stays visible behind another is invisible in a test and
// obvious on screen.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
constexpr auto barHeight = 28.f;

eacp::Graphics::MouseEvent mouseAt(Point position)
{
    auto event = eacp::Graphics::MouseEvent {};

    event.pos = position;
    event.downPos = position;

    return event;
}

struct Harness
{
    Harness()
    {
        host.setBounds({0.f, 0.f, 300.f, 200.f});
        host.setRootComponent(root);

        root.setBounds({0.f, 0.f, 300.f, 200.f});
        root.addAndMakeVisible(tabs);

        tabs.setBounds({0.f, 0.f, 300.f, 200.f});

        tabs.addTab("first", first);
        tabs.addTab("second", second);
        tabs.addTab("third", third);

        tabs.onChange = [this](int index)
        {
            lastChange = index;
            ++changes;
        };
    }

    void click(Point position)
    {
        host.mouseDown(mouseAt(position));
        host.mouseUp(mouseAt(position));
    }

    ComponentHost host;
    Component root;

    TabbedComponent tabs;
    Component first;
    Component second;
    Component third;

    int lastChange = -2;
    int changes = 0;
};
} // namespace

auto tTabsFirstTabShows = test("Tabs/theFirstTabAddedIsTheOneShowing") = []
{
    auto harness = Harness {};

    check(harness.tabs.getNumTabs() == 3);
    check(harness.tabs.getCurrentTabIndex() == 0);
    check(harness.tabs.getCurrentPage() == &harness.first);

    check(harness.first.isVisible());
    check(!harness.second.isVisible());
    check(!harness.third.isVisible());

    check(harness.changes == 0, "and nobody was told, nobody having asked");
};

auto tTabsPageFillsWhatTheBarLeaves = test("Tabs/thePageFillsWhatTheBarLeaves") = []
{
    auto harness = Harness {};

    auto page = harness.first.getBounds();

    check(page.y == barHeight);
    check(page.w == 300.f && page.h == 200.f - barHeight);
    check(harness.tabs.getTabBar().getBounds().h == barHeight);
};

auto tTabsSettingTheTabIsSilent = test("Tabs/settingTheTabReportsOnlyWhenAsked") = []
{
    auto harness = Harness {};

    harness.tabs.setCurrentTabIndex(1);

    check(harness.tabs.getCurrentTabIndex() == 1);
    check(harness.changes == 0);

    check(!harness.first.isVisible(), "the page swaps either way");
    check(harness.second.isVisible());

    harness.tabs.setCurrentTabIndex(2, true);

    check(harness.changes == 1 && harness.lastChange == 2);
    check(harness.third.isVisible());
    check(harness.third.getBounds().y == barHeight,
          "and a page shown for the first time is laid out");

    // An index naming no tab is refused rather than clamped.
    harness.tabs.setCurrentTabIndex(7, true);

    check(harness.tabs.getCurrentTabIndex() == 2);
    check(harness.changes == 1);
};

auto tTabsClickingATabSelectsIt =
    test("Tabs/clickingATabSelectsItAndSwapsThePage") = []
{
    auto harness = Harness {};

    // Three tabs across three hundred points, so the middle one is the middle
    // hundred.
    harness.click({150.f, 10.f});

    check(harness.tabs.getCurrentTabIndex() == 1);
    check(harness.changes == 1 && harness.lastChange == 1);
    check(harness.second.isVisible() && !harness.first.isVisible());

    // The one already showing is not a change.
    harness.click({150.f, 10.f});

    check(harness.changes == 1);
};

auto tTabsBarKeepsOneSelected = test("Tabs/aBarHasExactlyOneTabSelected") = []
{
    auto bar = TabBar {};
    auto changes = 0;

    bar.setBounds({0.f, 0.f, 300.f, barHeight});
    bar.onChange = [&changes](int) { ++changes; };

    check(bar.getCurrentTabIndex() == -1, "until there is a tab to select");

    bar.addTab("first");

    check(bar.getCurrentTabIndex() == 0);
    check(changes == 0);

    bar.addTab("second");

    check(bar.getCurrentTabIndex() == 0, "and a later tab does not steal it");
    check(bar.getTabName(1) == "second");

    bar.setCurrentTabIndex(1, true);

    check(bar.getCurrentTabIndex() == 1 && changes == 1);

    bar.clearTabs();

    check(bar.getNumTabs() == 0 && bar.getCurrentTabIndex() == -1);
};

auto tTabsBarTabBounds = test("Tabs/theTabsDivideTheBarBetweenThem") = []
{
    auto bar = TabBar {};

    bar.setBounds({0.f, 0.f, 300.f, barHeight});
    bar.addTab("first");
    bar.addTab("second");

    check(bar.getTabBounds(0).w == 150.f);
    check(bar.getTabBounds(1).x == 150.f);
    check(bar.getTabBounds(2).isEmpty(), "and a tab that is not there is nowhere");

    check(bar.getTabAt({10.f, 10.f}) == 0);
    check(bar.getTabAt({200.f, 10.f}) == 1);
    check(bar.getTabAt({200.f, 100.f}) == -1, "below the bar is no tab at all");
};
