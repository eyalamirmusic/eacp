#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

// Which components a change actually repaints.
//
// The tier's claim is that paint() runs where repaint() was called and nowhere
// else -- so an animation costs the paint of the one component that is animating
// however large the tree around it is. What makes that true is two bits per
// component and one invariant: a component with either bit set has every one of
// its ancestors marked, or the walk that skips clean subtrees would step over it.
//
// The interesting cases are the ones where the answer is *no* repaint: a move, a
// reorder, a visibility change. Each of those changes the picture without
// changing any recording in it, and paying a paint for one is the thing this
// whole arrangement exists to stop.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
bool hasDevice()
{
    return GPU::Device::shared().isValid();
}

struct Counter final : Component
{
    void paint(UI::Graphics&) override { ++paints; }

    int paints = 0;
};

// A host with a root and two children, painted once so that everything in it is
// settled -- which is the state every question below is asked from.
struct Tree
{
    Tree()
    {
        host.setRootComponent(root);
        root.setBounds({0.f, 0.f, 200.f, 100.f});

        root.addAndMakeVisible(first);
        root.addAndMakeVisible(second);

        first.setBounds({0.f, 0.f, 50.f, 50.f});
        second.setBounds({50.f, 0.f, 50.f, 50.f});

        host.paintDirtyComponents();
    }

    ComponentHost host;
    Counter root;
    Counter first;
    Counter second;
};
} // namespace

auto tEverythingStartsStale =
    test("Repaint/aComponentThatHasNeverPaintedOwesOne") = []
{
    auto component = Component {};

    check(component.needsRepaint());
};

auto tSettledTreePaintsNothing = test("Repaint/aSettledTreePaintsNothing") = []
{
    if (!hasDevice())
        return;

    auto tree = Tree {};

    check(tree.root.paints == 1, "everything was painted once on the way in");
    check(tree.first.paints == 1);

    check(tree.host.paintDirtyComponents() == 0, "and nothing after that");
    check(tree.root.paints == 1);
};

auto tOnlyTheOneThatAsked = test("Repaint/onlyTheComponentThatAskedIsPainted") = []
{
    if (!hasDevice())
        return;

    auto tree = Tree {};

    tree.first.repaint();

    check(tree.host.paintDirtyComponents() == 1);

    check(tree.first.paints == 2);
    check(tree.second.paints == 1, "its sibling is not asked");
    check(tree.root.paints == 1, "and neither is the parent holding it");
};

auto tAncestorsAreMarked = test("Repaint/aRepaintDeepInATreeIsReachedByTheWalk") = []
{
    if (!hasDevice())
        return;

    auto tree = Tree {};

    auto deep = Counter {};
    tree.first.addAndMakeVisible(deep);
    deep.setBounds({0.f, 0.f, 10.f, 10.f});

    // Added to a tree that had settled: nothing above it knew there was anything
    // left to do, and the walk only descends where there is.
    check(tree.host.paintDirtyComponents() == 1);
    check(deep.paints == 1);

    deep.repaint();

    check(tree.host.paintDirtyComponents() == 1);
    check(deep.paints == 2);
};

auto tMovingIsNotRepainting = test("Repaint/movingAComponentDoesNotRepaintIt") = []
{
    if (!hasDevice())
        return;

    auto tree = Tree {};

    tree.first.setBounds({20.f, 20.f, 50.f, 50.f});

    check(!tree.first.needsRepaint(), "the recording is in its own space");
    check(tree.host.paintDirtyComponents() == 0);
    check(tree.first.paints == 1);
};

auto tResizingIsRepainting = test("Repaint/resizingAComponentRepaintsIt") = []
{
    if (!hasDevice())
        return;

    auto tree = Tree {};

    tree.first.setBounds({0.f, 0.f, 80.f, 50.f});

    check(tree.first.needsRepaint(), "a paint() draws in local bounds");
    check(tree.host.paintDirtyComponents() == 1);
    check(tree.first.paints == 2);
};

auto tReorderingIsNotRepainting =
    test("Repaint/reorderingSiblingsRepaintsNeither") = []
{
    if (!hasDevice())
        return;

    auto tree = Tree {};

    tree.first.toFront();

    check(tree.host.paintDirtyComponents() == 0, "order is read as the frame draws");
    check(tree.first.paints == 1);
    check(tree.second.paints == 1);
};

auto tHidingIsNotRepainting = test("Repaint/hidingAndShowingRepaintsNothing") = []
{
    if (!hasDevice())
        return;

    auto tree = Tree {};

    tree.first.setVisible(false);
    check(tree.host.paintDirtyComponents() == 0);

    tree.first.setVisible(true);
    check(tree.host.paintDirtyComponents() == 0,
          "it replays what it was hidden with");
    check(tree.first.paints == 1);
};

auto tHiddenBeforeItEverPainted =
    test("Repaint/aComponentHiddenBeforeItEverPaintedIsPaintedWhenItIsShown") = []
{
    if (!hasDevice())
        return;

    auto tree = Tree {};

    auto late = Counter {};
    late.setVisible(false);
    tree.root.addChildComponent(late);
    late.setBounds({0.f, 0.f, 10.f, 10.f});

    check(tree.host.paintDirtyComponents() == 0,
          "a hidden component is not painted");
    check(late.paints == 0);

    late.setVisible(true);

    // The walk steps over hidden subtrees, so what one of them is owed has to be
    // carried up rather than forgotten -- otherwise nothing would ever reach it.
    check(tree.host.paintDirtyComponents() == 1);
    check(late.paints == 1);
};

auto tRepaintDuringPaint =
    test("Repaint/aRepaintFromInsideAPaintIsAnsweredByTheNextOne") = []
{
    if (!hasDevice())
        return;

    struct Restless final : Component
    {
        void paint(UI::Graphics&) override
        {
            ++paints;

            if (paints < 3)
                repaint();
        }

        int paints = 0;
    };

    auto host = ComponentHost {};
    auto restless = Restless {};

    host.setRootComponent(restless);
    restless.setBounds({0.f, 0.f, 50.f, 50.f});

    check(host.paintDirtyComponents() == 1);
    check(restless.paints == 1, "the request is for the next frame, not this one");

    check(host.paintDirtyComponents() == 1);
    check(restless.paints == 2);

    check(host.paintDirtyComponents() == 1);
    check(restless.paints == 3);

    check(host.paintDirtyComponents() == 0, "and it settles when it stops asking");
};

namespace
{
struct Shaped final : Component
{
    Shaped()
        : shape(*this)
    {
    }

    void paint(UI::Graphics& g) override
    {
        ++paints;
        g.fillPath(shape);
    }

    PathShape shape;
    int paints = 0;
};

// A shape on a settled host, which is the state every question below is asked
// from: whatever the call did, it did it to something that owed no paint.
struct ShapedTree
{
    ShapedTree()
    {
        host.setRootComponent(shaped);
        shaped.setBounds({0.f, 0.f, 50.f, 50.f});
        host.paintDirtyComponents();
    }

    ComponentHost host;
    Shaped shaped;
};

GPUWidgets::Path rect(const Rect& bounds)
{
    auto path = GPUWidgets::Path {};
    path.addRect(bounds);

    return path;
}
} // namespace

auto tPathShapeRepaints =
    test("Repaint/settingAPathRepaintsTheComponentThatDrawsIt") = []
{
    if (!hasDevice())
        return;

    auto tree = ShapedTree {};

    tree.shaped.shape.setPath(rect({0.f, 0.f, 10.f, 10.f}));

    // The recorded quad carries the shape's bounds and its rect of the atlas,
    // and both are about to be different -- so the component that drew it has to
    // be asked again, and nothing but the shape itself knows that.
    check(tree.shaped.needsRepaint());
    check(tree.host.paintDirtyComponents() == 1);
};

// The rung-3 claim, and the reason a layout may rebuild every path it owns
// without first working out whether it had to: a shape set to the geometry it
// already holds is not a shape that changed.
auto tSamePathIsNotAChange =
    test("Repaint/settingTheGeometryAShapeAlreadyHoldsDoesNothing") = []
{
    if (!hasDevice())
        return;

    auto tree = ShapedTree {};

    tree.shaped.shape.setPath(rect({0.f, 0.f, 10.f, 10.f}));
    tree.host.paintDirtyComponents();

    check(!tree.shaped.needsRepaint(), "settled");

    // Built again from scratch rather than the same object handed back, which
    // is what a resized() does: the same arithmetic over the same inputs, so
    // the same bits.
    tree.shaped.shape.setPath(rect({0.f, 0.f, 10.f, 10.f}));

    check(!tree.shaped.needsRepaint(), "the same shape is not a new shape");
    check(tree.host.paintDirtyComponents() == 0);
};

auto tDifferentPathIsAChange = test("Repaint/aShapeThatMovedOrGrewIsAChange") = []
{
    if (!hasDevice())
        return;

    auto tree = ShapedTree {};

    tree.shaped.shape.setPath(rect({0.f, 0.f, 10.f, 10.f}));
    tree.host.paintDirtyComponents();

    tree.shaped.shape.setPath(rect({0.f, 0.f, 10.f, 10.5f}));

    check(tree.shaped.needsRepaint(), "half a point is still a different shape");
    check(tree.host.paintDirtyComponents() == 1);
};

// The rule is not part of the geometry, but it decides what the geometry means,
// so it is part of the key.
auto tFillRuleIsPartOfTheKey =
    test("Repaint/theSamePointsUnderADifferentRuleAreADifferentShape") = []
{
    if (!hasDevice())
        return;

    auto tree = ShapedTree {};

    auto ring = rect({0.f, 0.f, 10.f, 10.f});
    ring.addRect({2.f, 2.f, 6.f, 6.f});

    tree.shaped.shape.setPath(ring, GPUWidgets::FillRule::NonZero);
    tree.host.paintDirtyComponents();

    tree.shaped.shape.setPath(ring, GPUWidgets::FillRule::EvenOdd);

    check(tree.shaped.needsRepaint(), "one of these has a hole in it");
    check(tree.host.paintDirtyComponents() == 1);
};

// Clearing drops the geometry rather than remembering it, so the shape that was
// there is not mistaken for the shape that is coming back.
auto tClearedThenSetAgainRepaints =
    test("Repaint/aClearedShapeGivenItsOldPathBackIsAChange") = []
{
    if (!hasDevice())
        return;

    auto tree = ShapedTree {};

    tree.shaped.shape.setPath(rect({0.f, 0.f, 10.f, 10.f}));
    tree.host.paintDirtyComponents();

    tree.shaped.shape.clear();
    tree.host.paintDirtyComponents();

    tree.shaped.shape.setPath(rect({0.f, 0.f, 10.f, 10.f}));

    check(tree.shaped.needsRepaint());
    check(tree.host.paintDirtyComponents() == 1);
};
