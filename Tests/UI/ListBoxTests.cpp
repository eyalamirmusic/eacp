#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

// A list of rows that are not components: the model paints them and the box
// works out which one a point landed on.
//
// Which is where a list like this is actually wrong. The row a click selects,
// the row a scroll offset puts on screen and the row a column is measured
// against are three separate pieces of arithmetic over the same numbers, and
// nothing but a test says they agree -- a header counted twice, or a scroll
// offset applied the wrong way round, still draws a plausible list.
//
// Nothing is rendered: the model records what it was asked to paint rather than
// painting it.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
constexpr auto rowHeight = 20.f;

eacp::Graphics::MouseEvent mouseAt(Point position, int clickCount = 1)
{
    auto event = eacp::Graphics::MouseEvent {};

    event.pos = position;
    event.downPos = position;
    event.clickCount = clickCount;

    return event;
}

struct Model final : ListBoxModel
{
    int getNumRows() override { return rows; }

    void paintRow(UI::Graphics&, int, const Rect&, bool) override {}

    void selectedRowChanged(int row) override
    {
        lastSelected = row;
        ++selections;
    }

    void rowDoubleClicked(int row) override { lastDoubleClicked = row; }

    int rows = 20;
    int lastSelected = -2;
    int selections = 0;
    int lastDoubleClicked = -1;
};

struct Harness
{
    Harness()
    {
        host.setBounds({0.f, 0.f, 200.f, 100.f});
        host.setRootComponent(root);

        root.setBounds({0.f, 0.f, 200.f, 100.f});
        root.addAndMakeVisible(list);

        list.setBounds({0.f, 0.f, 200.f, 100.f});
        list.setRowHeight(rowHeight);
        list.setModel(&model);
    }

    void click(Point position, int clickCount = 1)
    {
        host.mouseDown(mouseAt(position, clickCount));
        host.mouseUp(mouseAt(position, clickCount));
    }

    ComponentHost host;
    Component root;
    Model model;
    ListBox list;
};
} // namespace

auto tListClickSelects = test("ListBox/clickingARowSelectsIt") = []
{
    auto harness = Harness {};

    check(harness.list.getNumRows() == 20,
          "the count is read when the model is set");
    check(harness.list.getSelectedRow() == -1);

    harness.click({50.f, 50.f});

    check(harness.list.getSelectedRow() == 2);
    check(harness.model.lastSelected == 2 && harness.model.selections == 1);

    // Past the last row of a short list is not a row.
    harness.model.rows = 2;
    harness.list.updateContent();
    harness.click({50.f, 90.f});

    check(harness.list.getSelectedRow() == 1, "which is where the clamp left it");
    check(harness.model.selections == 1, "and the click selected nothing");
};

auto tListSelectingIsSilent =
    test("ListBox/settingTheSelectionReportsOnlyWhenAsked") = []
{
    auto harness = Harness {};

    harness.list.setSelectedRow(4);

    check(harness.list.getSelectedRow() == 4);
    check(harness.model.selections == 0);

    harness.list.setSelectedRow(5, true);

    check(harness.model.selections == 1 && harness.model.lastSelected == 5);

    harness.list.setSelectedRow(99, true);

    check(harness.list.getSelectedRow() == 5, "a row that is not there is refused");
    check(harness.model.selections == 1);
};

auto tListDoubleClick = test("ListBox/aSecondClickOnARowIsADoubleClick") = []
{
    auto harness = Harness {};

    harness.click({50.f, 50.f});
    harness.click({50.f, 50.f}, 2);

    check(harness.model.lastDoubleClicked == 2);
    check(harness.model.selections == 1,
          "and selecting what is selected is not new");
};

auto tListUpdateContentClamps =
    test("ListBox/updateContentPutsTheSelectionBackInRange") = []
{
    auto harness = Harness {};

    harness.list.setSelectedRow(18);
    harness.list.scrollToRow(19);

    check(harness.list.getScrollPosition() == 300.f);

    harness.model.rows = 5;
    harness.list.updateContent();

    check(harness.list.getNumRows() == 5);
    check(harness.list.getSelectedRow() == 4);
    check(harness.list.getScrollPosition() == 0.f, "and the scroll with it");
    check(harness.model.selections == 0, "a row that vanished is not a change");

    harness.model.rows = 0;
    harness.list.updateContent();

    check(harness.list.getSelectedRow() == -1, "an empty list selects nothing");
};

auto tListScrollToRow = test("ListBox/scrollingToARowMovesAsFarAsItHasTo") = []
{
    auto harness = Harness {};

    // Five rows fit in a hundred points, so the top five are already there.
    harness.list.scrollToRow(3);

    check(harness.list.getScrollPosition() == 0.f);

    harness.list.scrollToRow(7);

    check(harness.list.getScrollPosition() == 60.f, "just far enough to show it");

    harness.list.scrollToRow(2);

    check(harness.list.getScrollPosition() == 40.f, "and back from the other edge");

    harness.list.setScrollPosition(1000.f);

    check(harness.list.getScrollPosition() == 300.f, "never past the last row");
};

auto tListScrolledRowsAreHit =
    test("ListBox/aScrolledListSelectsWhatIsUnderThePointer") = []
{
    auto harness = Harness {};

    harness.list.setScrollPosition(50.f);
    harness.click({50.f, 10.f});

    check(harness.list.getSelectedRow() == 3, "the offset counts once, not twice");
    check(harness.list.getRowBounds(3).y == 10.f);
};

auto tListWheelScrolls = test("ListBox/theWheelScrollsTheRows") = []
{
    auto harness = Harness {};

    auto wheel = mouseAt({50.f, 50.f});
    wheel.delta = {0.f, -1.f};

    harness.host.mouseWheel(wheel);

    check(harness.list.getScrollPosition() == 40.f, "a notched wheel moves lines");

    harness.model.rows = 1;
    harness.list.updateContent();
    harness.host.mouseWheel(wheel);

    check(harness.list.getScrollPosition() == 0.f,
          "and a list that fits does not scroll at all");
};

// The header is the difference between a list and a table, and it is the thing
// the row arithmetic has to leave room for.
auto tListColumns = test("ListBox/columnsSplitARowAndLeaveRoomForTheHeader") = []
{
    auto harness = Harness {};

    harness.list.setColumns({{"time", 60.f}, {"channel", 40.f}, {"message", 100.f}});
    harness.list.setHeaderHeight(24.f);

    check(harness.list.getHeaderHeight() == 24.f);
    check(harness.list.getRowArea().y == 24.f);
    check(harness.list.getRowBounds(0).y == 24.f, "the first row is under it");

    auto row = harness.list.getRowBounds(1);
    auto second = harness.list.getColumnBounds(1, row);

    check(second.x == 60.f && second.w == 40.f);
    check(second.y == row.y && second.h == row.h);

    check(harness.list.getColumnBounds(3, row).isEmpty());

    // And the click that lands on that row has to agree with where it is drawn.
    harness.click({50.f, 50.f});

    check(harness.list.getSelectedRow() == 1);

    harness.click({50.f, 10.f});

    check(harness.list.getSelectedRow() == 1,
          "and a click on the header is not one");
};
