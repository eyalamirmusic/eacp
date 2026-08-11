#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <string>

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
struct Bin final : Component
{
    Bin()
    {
        target.itemDragEnter = [this](const DragInfo&) { ++entered; };
        target.itemDragExit = [this](const DragInfo&) { ++exited; };
        target.itemDragMove = [this](const DragInfo&) { ++moved; };
        target.itemDropped = [this](const DragInfo& info)
        {
            ++dropped;
            lastId = info.itemId;
            lastPosition = info.position;
        };
    }

    DragAndDropTarget target {*this};

    int entered = 0;
    int exited = 0;
    int moved = 0;
    int dropped = 0;
    int lastId = 0;
    Point lastPosition;
};

struct Harness
{
    Harness()
    {
        board.setBounds({0.f, 0.f, 200.f, 100.f});

        board.addAndMakeVisible(left);
        board.addAndMakeVisible(right);
        board.addAndMakeVisible(source);

        left.setBounds({0.f, 0.f, 100.f, 100.f});
        right.setBounds({100.f, 0.f, 100.f, 100.f});
        source.setBounds({10.f, 10.f, 20.f, 20.f});
    }

    void startDrag(int itemId = 7)
    {
        auto info = DragInfo {};
        info.type = "card";
        info.itemId = itemId;

        dragging.startDragging(
            info, source, {40.f, 20.f}, [](UI::Graphics&, const Rect&) {});
    }

    Component board;
    Bin left;
    Bin right;
    Component source;

    DragAndDropContainer dragging {board};
};
} // namespace

auto tDragReachesATarget =
    test("DragAndDrop/aDragEntersTheTargetUnderThePointer") = []
{
    auto harness = Harness {};

    harness.startDrag();
    harness.dragging.dragTo({50.f, 50.f});

    check(harness.left.entered == 1);
    check(harness.left.moved == 1);
    check(harness.right.entered == 0);
};

auto tMovingBetweenTargets = test("DragAndDrop/leavingOneTargetEntersTheOther") = []
{
    auto harness = Harness {};

    harness.startDrag();
    harness.dragging.dragTo({50.f, 50.f});
    harness.dragging.dragTo({150.f, 50.f});

    check(harness.left.exited == 1, "and the first is told it was left");
    check(harness.right.entered == 1);
};

auto tDropExitsFirst = test("DragAndDrop/aDropLeavesTheTargetBeforeDropping") = []
{
    auto harness = Harness {};

    harness.startDrag();
    harness.dragging.dragTo({50.f, 50.f});
    harness.dragging.drop({50.f, 50.f});

    check(harness.left.exited == 1);
    check(harness.left.dropped == 1);
    check(harness.left.lastId == 7);
};

auto tDropReportsThePositionInTheTarget =
    test("DragAndDrop/aDropReportsWhereItLandedInTheTarget") = []
{
    auto harness = Harness {};

    harness.startDrag();
    harness.dragging.drop({150.f, 40.f});

    check(harness.right.dropped == 1);
    check(harness.right.lastPosition.x == 50.f, "the right bin starts at x = 100");
    check(harness.right.lastPosition.y == 40.f);
};

auto tDropOutsideAnyTarget =
    test("DragAndDrop/aDropOnNothingEndsTheDragQuietly") = []
{
    auto harness = Harness {};

    harness.startDrag();
    harness.dragging.dragTo({50.f, 50.f});
    harness.dragging.drop({500.f, 500.f});

    check(harness.left.dropped == 0);
    check(harness.right.dropped == 0);
    check(harness.left.exited == 1,
          "and whatever it was over is still told it left");
    check(!harness.dragging.isDragging());
};

auto tRefusedDragsCarryOn = test("DragAndDrop/aTargetThatRefusesIsPassedOver") = []
{
    auto harness = Harness {};

    harness.left.target.isInterestedIn = [](const DragInfo&) { return false; };

    harness.startDrag();
    harness.dragging.dragTo({50.f, 50.f});

    check(harness.left.entered == 0);

    harness.dragging.drop({50.f, 50.f});

    check(harness.left.dropped == 0);
};

auto tInterestIsAskedOfThePayload =
    test("DragAndDrop/aTargetDecidesFromWhatIsBeingDragged") = []
{
    auto harness = Harness {};
    auto seen = std::string {};

    harness.left.target.isInterestedIn = [&seen](const DragInfo& info)
    {
        seen = info.type;
        return info.itemId == 1;
    };

    harness.startDrag(2);
    harness.dragging.drop({50.f, 50.f});

    check(seen == "card");
    check(harness.left.dropped == 0, "the id was not one it wanted");
};

auto tImageIsAChildWhileDragging =
    test("DragAndDrop/theDraggedImageIsInTheTreeOnlyWhileTheDragIs") = []
{
    auto harness = Harness {};

    auto before = harness.board.getChildren().size();

    harness.startDrag();

    check(harness.board.getChildren().size() == before + 1);
    check(harness.dragging.isDragging());

    harness.dragging.drop({50.f, 50.f});

    check(harness.board.getChildren().size() == before);
    check(!harness.dragging.isDragging());
};

auto tImageIsTransparentToTheSearch =
    test("DragAndDrop/theDraggedImageIsNotItselfATarget") = []
{
    auto harness = Harness {};

    harness.startDrag();

    harness.dragging.dragTo({50.f, 50.f});
    harness.dragging.drop({50.f, 50.f});

    check(harness.left.dropped == 1);
};

// The image and the drag state are torn down before the drop callback runs, so
// a callback that destroys the source leaves nothing dangling.
auto tDropMayDestroyTheSource =
    test("DragAndDrop/aDropCanDestroyWhatWasBeingDragged") = []
{
    auto board = Component {};
    board.setBounds({0.f, 0.f, 100.f, 100.f});

    auto bin = Bin {};
    board.addAndMakeVisible(bin);
    bin.setBounds({0.f, 0.f, 100.f, 100.f});

    auto container = DragAndDropContainer {board};
    auto source = makeOwned<Component>();

    board.addAndMakeVisible(*source);
    source->setBounds({0.f, 0.f, 10.f, 10.f});

    bin.target.itemDropped = [&](const DragInfo& info)
    {
        check(info.itemId == 3);
        source.reset();
    };

    auto info = DragInfo {};
    info.itemId = 3;

    container.startDragging(
        info, *source, {10.f, 10.f}, [](UI::Graphics&, const Rect&) {});
    container.drop({50.f, 50.f});

    check(source == nullptr);
    check(!container.isDragging());
};

auto tDeletingTheSourceCancels = test("DragAndDrop/losingTheSourceEndsTheDrag") = []
{
    auto board = Component {};
    board.setBounds({0.f, 0.f, 100.f, 100.f});

    auto bin = Bin {};
    board.addAndMakeVisible(bin);
    bin.setBounds({0.f, 0.f, 100.f, 100.f});

    auto container = DragAndDropContainer {board};

    {
        auto source = Component {};
        board.addAndMakeVisible(source);

        auto info = DragInfo {};
        info.itemId = 5;

        container.startDragging(
            info, source, {10.f, 10.f}, [](UI::Graphics&, const Rect&) {});
        container.dragTo({50.f, 50.f});

        check(container.isDragging());
        check(bin.entered == 1);
    }

    check(!container.isDragging(), "the drag went with the component it came from");
    check(bin.exited == 1, "and the target it was over was told");

    container.drop({50.f, 50.f});

    check(bin.dropped == 0);
};

auto tStartingASecondDragEndsTheFirst =
    test("DragAndDrop/startingAnotherDragCleansUpTheFirst") = []
{
    auto harness = Harness {};

    harness.startDrag(1);
    harness.dragging.dragTo({50.f, 50.f});
    harness.startDrag(2);

    check(harness.board.getChildren().size() == 4, "one image, not two");
    check(harness.dragging.getCurrentDrag().itemId == 2);
};

auto tCancelling = test("DragAndDrop/aCancelledDragDropsNothing") = []
{
    auto harness = Harness {};

    harness.startDrag();
    harness.dragging.dragTo({50.f, 50.f});
    harness.dragging.cancelDrag();

    check(!harness.dragging.isDragging());
    check(harness.left.exited == 1);
    check(harness.left.dropped == 0);
};

auto tHiddenTargetsAreSkipped = test("DragAndDrop/aHiddenTargetTakesNothing") = []
{
    auto harness = Harness {};

    harness.left.setVisible(false);

    harness.startDrag();
    harness.dragging.drop({50.f, 50.f});

    check(harness.left.dropped == 0);
};

auto tContainerIsFoundByWalkingUp =
    test("DragAndDrop/aSourceFindsTheContainerAboveIt") = []
{
    auto harness = Harness {};

    check(harness.source.findDragContainer() == &harness.dragging);
    check(harness.board.findDragContainer() == &harness.dragging);

    auto loose = Component {};

    check(loose.findDragContainer() == nullptr, "and a tree without one has none");
};
