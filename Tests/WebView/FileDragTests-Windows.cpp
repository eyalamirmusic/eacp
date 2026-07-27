// The Windows file drag-out runs inside SHDoDragDrop's blocking modal loop, so
// the streaming callbacks (onFileDragMoved / onFileDragEnded) hang off the
// FileDragSource / FileDragTarget COM objects that loop talks to. These tests
// drive those objects directly — the cursor reader is injected, so no live
// drag, cursor, or window is needed.

#include <eacp/WebView/WebView/FileDrag-Windows.h>
#include <NanoTest/NanoTest.h>

#include <vector>

using namespace nano;
using namespace eacp::Graphics;

namespace
{
WebView::FileDragPoint point(double x, double y, bool inside)
{
    return {.x = x, .y = y, .inside = inside};
}

RECT clientRect(long width, long height)
{
    return {.left = 0, .top = 0, .right = width, .bottom = height};
}
} // namespace

auto tPointMapsPhysicalToCssPixels =
    test("FileDrag/pointMapsPhysicalPixelsToCssPixels") = []
{
    // 200% DPI: a cursor at physical (100, 50) is CSS (50, 25) — the same
    // coordinates the page reads as clientX/clientY.
    auto mapped = toFileDragPoint({.x = 100, .y = 50}, clientRect(200, 100), 2.f);

    check(mapped.x == 50.0);
    check(mapped.y == 25.0);
    check(mapped.inside);
};

auto tPointOutsideClientRect = test("FileDrag/pointOutsideClientRectIsOutside") = []
{
    auto rect = clientRect(200, 100);

    check(!toFileDragPoint({.x = -1, .y = 50}, rect, 1.f).inside);
    // PtInRect excludes the right/bottom edge, matching a client rect's bounds.
    check(!toFileDragPoint({.x = 200, .y = 50}, rect, 1.f).inside);
    check(!toFileDragPoint({.x = 100, .y = 100}, rect, 1.f).inside);
    check(toFileDragPoint({.x = 0, .y = 0}, rect, 1.f).inside);
};

auto tZeroDpiFallsBackUnscaled = test("FileDrag/zeroDpiScaleFallsBackToUnscaled") = []
{
    auto mapped = toFileDragPoint({.x = 40, .y = 30}, clientRect(200, 100), 0.f);

    check(mapped.x == 40.0);
    check(mapped.y == 30.0);
};

auto tHeldButtonStreamsCursor =
    test("FileDrag/heldLeftButtonContinuesAndStreamsCursor") = []
{
    auto moves = std::vector<WebView::FileDragPoint> {};
    auto source = FileDragSource {[] { return point(12, 34, true); },
                                  [&](WebView::FileDragPoint p)
                                  { moves.push_back(p); }};

    check(source.QueryContinueDrag(FALSE, MK_LBUTTON) == S_OK);
    check(source.QueryContinueDrag(FALSE, MK_LBUTTON) == S_OK);

    check(moves.size() == 2);
    check(moves[0].x == 12.0);
    check(moves[0].y == 34.0);
    check(moves[0].inside);
};

auto tReleaseIsTheDrop = test("FileDrag/leftButtonReleaseIsTheDrop") = []
{
    auto moves = 0;
    auto source = FileDragSource {[] { return point(0, 0, true); },
                                  [&](WebView::FileDragPoint) { ++moves; }};

    check(source.QueryContinueDrag(FALSE, 0) == DRAGDROP_S_DROP);
    check(moves == 0);
};

auto tEscapeCancels = test("FileDrag/escapeCancels") = []
{
    auto source = FileDragSource {[] { return point(0, 0, true); },
                                  [](WebView::FileDragPoint) {}};

    check(source.QueryContinueDrag(TRUE, MK_LBUTTON) == DRAGDROP_S_CANCEL);
};

auto tRightButtonCancels = test("FileDrag/rightButtonCancels") = []
{
    auto source = FileDragSource {[] { return point(0, 0, true); },
                                  [](WebView::FileDragPoint) {}};

    check(source.QueryContinueDrag(FALSE, MK_LBUTTON | MK_RBUTTON)
          == DRAGDROP_S_CANCEL);
};

auto tSourceUsesDefaultCursors = test("FileDrag/sourceUsesDefaultCursors") = []
{
    auto source = FileDragSource {[] { return point(0, 0, true); },
                                  [](WebView::FileDragPoint) {}};

    check(source.GiveFeedback(DROPEFFECT_COPY) == DRAGDROP_S_USEDEFAULTCURSORS);
};

auto tTargetReportsCopy = test("FileDrag/targetReportsCopyForCursorFeedback") = []
{
    // Without a registered target the shell paints the ⊘ "no drop" badge over
    // our own window; the target's one job is to report COPY everywhere so the
    // cursor matches what the page will actually do with the drop.
    auto target = FileDragTarget {};

    auto effect = DWORD {DROPEFFECT_NONE};
    check(target.DragEnter(nullptr, 0, {}, &effect) == S_OK);
    check(effect == DROPEFFECT_COPY);

    effect = DROPEFFECT_NONE;
    check(target.DragOver(0, {}, &effect) == S_OK);
    check(effect == DROPEFFECT_COPY);

    effect = DROPEFFECT_NONE;
    check(target.Drop(nullptr, 0, {}, &effect) == S_OK);
    check(effect == DROPEFFECT_COPY);

    check(target.DragLeave() == S_OK);
};

auto tComContract = test("FileDrag/comIdentityAndRefCount") = []
{
    // Born with one ref (the Attach convention in WebView-Windows.cpp); the
    // final Release deletes, so this test manages the object manually.
    auto* source = new FileDragSource {[] { return point(0, 0, true); },
                                       [](WebView::FileDragPoint) {}};

    void* asDropSource = nullptr;
    check(source->QueryInterface(IID_IDropSource, &asDropSource) == S_OK);
    check(asDropSource == static_cast<IDropSource*>(source));

    void* asDropTarget = nullptr;
    check(source->QueryInterface(IID_IDropTarget, &asDropTarget)
          == E_NOINTERFACE);
    check(asDropTarget == nullptr);

    // QueryInterface took a ref: 2 total. Releases count down to deletion.
    check(source->Release() == 1);
    check(source->Release() == 0);
};
