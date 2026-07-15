#include "Common.h"

using namespace nano;
using eacp::Graphics::clampedCornerRadius;
using eacp::Graphics::Path;
using eacp::Graphics::Rect;

// A rounded rect whose corners don't fit inside it is out of contract for every
// backend: CGPathAddRoundedRect asserted on it until macOS clamped it away, and
// the Windows path builder still runs its top edge backwards. Callers scaling a
// rect down to zero -- a progress bar filling from empty -- hit this on frame
// one, so the radius has to be fitted to the rect before it leaves eacp.
auto tRadiusFitsTheShortestEdge = test("RoundedRect/radiusIsFittedToTheRect") = []
{
    check(clampedCornerRadius({0.f, 0.f, 100.f, 20.f}, 6.f) == 6.f);
    check(clampedCornerRadius({0.f, 0.f, 12.f, 12.f}, 6.f) == 6.f);
};

auto tRadiusShrinksToHalfTheShortestEdge =
    test("RoundedRect/radiusShrinksToHalfTheShortestEdge") = []
{
    check(clampedCornerRadius({0.f, 0.f, 3.f, 4.f}, 2.f) == 1.5f);
    check(clampedCornerRadius({0.f, 0.f, 40.f, 3.f}, 6.f) == 1.5f);
    check(clampedCornerRadius({0.f, 0.f, 0.5f, 4.f}, 2.f) == 0.25f);
};

auto tRadiusOfAnEmptyRect = test("RoundedRect/emptyRectHasNoCorners") = []
{
    check(clampedCornerRadius({24.f, 80.f, 0.f, 4.f}, 2.f) == 0.f);
    check(clampedCornerRadius({24.f, 80.f, 40.f, 0.f}, 2.f) == 0.f);
    check(clampedCornerRadius({24.f, 80.f, -5.f, 4.f}, 2.f) == 0.f);
};

auto tNegativeRadius = test("RoundedRect/negativeRadiusHasNoCorners") = []
{ check(clampedCornerRadius({0.f, 0.f, 40.f, 20.f}, -6.f) == 0.f); };

// The shape the Librarian crash came in as: a 4pt progress bar whose fill keeps
// the track's 2pt radius while its width is still a fraction of a point.
auto tProgressBarFillFromEmpty =
    test("RoundedRect/progressBarFillFromEmptyIsDrawable") = []
{
    constexpr auto barHeight = 4.f;
    constexpr auto radius = barHeight / 2.f;

    auto track = Rect {24.f, 80.f, 472.f, barHeight};
    auto path = Path();

    for (auto fraction: {0.f, 0.0001f, 0.001f, 0.5f, 1.f})
    {
        auto fill = track.withWidth(track.w * fraction);
        check(clampedCornerRadius(fill, radius) * 2.f <= fill.w || fill.w <= 0.f);
        path.addRoundedRect(fill, radius);
    }

    check(path.getHandle() != nullptr);
};
