#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

// Files dragged in from the OS, as the host's native view reports them: which
// component in the tree hears about them, in its own space, and in what order.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
using Files = Vector<std::string>;

struct Slot final
    : Component
    , FileDropTarget
{
    bool isInterestedInFileDrop(const Files& files) override
    {
        return interested && !files.empty();
    }

    void fileDragEnter(const Files&, Point position) override
    {
        ++enters;
        last = position;
    }

    void fileDragMove(const Files&, Point position) override
    {
        ++moves;
        last = position;
    }

    void fileDragExit(const Files&) override { ++exits; }

    void filesDropped(const Files& files, Point position) override
    {
        ++drops;
        last = position;
        dropped = files;
    }

    bool interested = true;
    int enters = 0;
    int moves = 0;
    int exits = 0;
    int drops = 0;
    Point last;
    Files dropped;
};

struct Tree
{
    Tree()
    {
        host.setBounds({0.f, 0.f, 400.f, 300.f});
        host.setRootComponent(root);

        root.addAndMakeVisible(outer);
        outer.setBounds({100.f, 50.f, 200.f, 200.f});

        outer.addAndMakeVisible(plain);
        plain.setBounds({0.f, 0.f, 200.f, 200.f});

        plain.addAndMakeVisible(inner);
        inner.setBounds({20.f, 30.f, 50.f, 50.f});

        root.addAndMakeVisible(cover);
        cover.setBounds({300.f, 0.f, 100.f, 300.f});
    }

    ComponentHost host;
    Component root;
    Slot outer;
    Component plain;
    Slot inner;
    Slot cover;
    Files files {"/tmp/kick.wav"};
};

struct Transparent final : Component
{
    bool hitTest(Point) const override { return false; }
};
} // namespace

auto tHostAcceptsFileDrops = test("FileDrop/aHostAcceptsFileDropsByDefault") = []
{
    auto host = ComponentHost {};

    check(host.getProperties().acceptsFileDrops);
};

auto tDeepestInterestedTargetWins =
    test("FileDrop/theDeepestInterestedTargetUnderThePointerWins") = []
{
    auto tree = Tree {};

    check(tree.host.findFileDropTarget(tree.files, {130.f, 90.f}) == &tree.inner);
    check(tree.host.findFileDropTarget(tree.files, {110.f, 60.f}) == &tree.outer);
    check(tree.host.findFileDropTarget(tree.files, {50.f, 50.f}) == nullptr);
    check(tree.host.findFileDropTarget({}, {130.f, 90.f}) == nullptr);
};

auto tUninterestedAndHiddenAreSkipped =
    test("FileDrop/uninterestedAndHiddenTargetsAreSkipped") = []
{
    auto tree = Tree {};

    tree.inner.interested = false;
    check(tree.host.findFileDropTarget(tree.files, {130.f, 90.f}) == &tree.outer);

    tree.inner.interested = true;
    tree.inner.setVisible(false);
    check(tree.host.findFileDropTarget(tree.files, {130.f, 90.f}) == &tree.outer);

    tree.outer.setVisible(false);
    check(tree.host.findFileDropTarget(tree.files, {130.f, 90.f}) == nullptr);
};

auto tFrontSiblingWins =
    test("FileDrop/theFrontSiblingWinsWhereSiblingsOverlap") = []
{
    auto tree = Tree {};
    tree.cover.setBounds({0.f, 0.f, 400.f, 300.f});

    check(tree.host.findFileDropTarget(tree.files, {130.f, 90.f}) == &tree.cover);

    tree.outer.toFront();
    check(tree.host.findFileDropTarget(tree.files, {130.f, 90.f}) == &tree.inner);
};

auto tHitTestLetsDropsThrough =
    test("FileDrop/aHitTestOverrideLetsDropsThrough") = []
{
    auto tree = Tree {};
    auto glass = Transparent {};

    tree.root.addAndMakeVisible(glass);
    glass.setBounds({0.f, 0.f, 400.f, 300.f});

    check(tree.host.findFileDropTarget(tree.files, {130.f, 90.f}) == &tree.inner);
};

auto tClicksAreNotRequired =
    test("FileDrop/aTargetNeedNotInterceptClicksToTakeFiles") = []
{
    auto tree = Tree {};

    check(!tree.inner.getInterceptsMouseClicks());
    check(tree.host.findFileDropTarget(tree.files, {130.f, 90.f}) == &tree.inner);
};

auto tDragSequenceIsRouted =
    test("FileDrop/enterMoveExitAndDropReachTheRightTargets") = []
{
    auto tree = Tree {};
    auto& host = tree.host;

    check(host.fileDragEnter(tree.files, {130.f, 90.f}));
    check(tree.inner.enters == 1);
    check(tree.inner.last.x == 10.f && tree.inner.last.y == 10.f);

    check(host.fileDragMove(tree.files, {135.f, 95.f}));
    check(tree.inner.moves == 1);
    check(tree.inner.last.x == 15.f && tree.inner.last.y == 15.f);

    check(host.fileDragMove(tree.files, {110.f, 60.f}));
    check(tree.inner.exits == 1);
    check(tree.outer.enters == 1);
    check(tree.outer.moves == 0, "entering is not also a move");

    check(!host.fileDragMove(tree.files, {50.f, 50.f}), "nothing takes files here");
    check(tree.outer.exits == 1);

    host.fileDragMove(tree.files, {130.f, 90.f});
    host.fileDragExit();
    check(tree.inner.exits == 2);
};

auto tDropGoesToTheTargetUnderThePointer =
    test("FileDrop/aDropGoesToTheTargetUnderThePointerWithoutAnExit") = []
{
    auto tree = Tree {};
    auto& host = tree.host;

    host.fileDragEnter(tree.files, {130.f, 90.f});
    host.filesDropped(tree.files, {140.f, 100.f});

    check(tree.inner.drops == 1);
    check(tree.inner.exits == 0, "the drop is the last call, not an exit");
    check(tree.inner.last.x == 20.f && tree.inner.last.y == 20.f);
    check(tree.inner.dropped.size() == 1);
    check(tree.inner.dropped[0] == "/tmp/kick.wav");

    host.fileDragEnter(tree.files, {130.f, 90.f});
    host.filesDropped(tree.files, {110.f, 60.f});

    check(tree.inner.exits == 1, "a drop elsewhere exits the target it left");
    check(tree.outer.drops == 1);
    check(tree.inner.drops == 1);
};

auto tDeletedTargetIsForgotten =
    test("FileDrop/aTargetDeletedMidDragIsNotCalledAgain") = []
{
    auto tree = Tree {};
    auto doomed = std::make_unique<Slot>();

    tree.root.addAndMakeVisible(*doomed);
    doomed->setBounds({0.f, 0.f, 50.f, 50.f});

    check(tree.host.fileDragEnter(tree.files, {10.f, 10.f}));
    check(doomed->enters == 1);

    doomed.reset();

    check(!tree.host.fileDragMove(tree.files, {10.f, 10.f}));
    tree.host.fileDragExit();
};

auto tSafeAreaOffsetIsApplied =
    test("FileDrop/positionsAreMeasuredFromTheRootInsideTheSafeArea") = []
{
    auto tree = Tree {};

    tree.host.setSafeAreaInsets({10.f, 20.f, 0.f, 0.f});

    auto root = tree.host.getRootBounds();
    tree.host.fileDragEnter(tree.files, {130.f + root.x, 90.f + root.y});

    check(tree.inner.enters == 1);
    check(tree.inner.last.x == 10.f && tree.inner.last.y == 10.f);
};
