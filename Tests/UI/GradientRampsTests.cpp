#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <string>

// What the ramp texture shares, and what it does when it cannot share any more.
//
// Sharing is the whole point of it. A gradient evaluated from its stops in the
// fragment stage would be a uniform block per gradient and a batch break between
// two shapes filled differently; baked into a row of one texture it is a fetch,
// and a document painting fifty shapes from one definition costs one row. So the
// key has to be the colours and nothing else -- not the shape, not the axis, and
// not the spread, which the shader folds into the coordinate before it reads.
//
// And the ceiling matters for the same reason CoverageAtlas's does: the texture
// cannot grow, because a draw already recorded this frame would be holding the
// one it replaced. Past it a gradient falls back to its flat colour, which is a
// picture missing its shading rather than missing a shape -- and still something
// that happened silently, so it is counted.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
// The ramps own a texture, so they need a device - but no window, no pass and no
// draw: baking a row is arithmetic over a stop list.
bool hasDevice()
{
    return GPU::Device::shared().isValid();
}

Gradient twoStop(const Color& from, const Color& to)
{
    auto gradient = Gradient {};
    gradient.end = {1.f, 0.f};
    gradient.stops = {{from, 0.f}, {to, 1.f}};

    return gradient;
}

const auto red = Color {1.f, 0.f, 0.f, 1.f};
const auto blue = Color {0.f, 0.f, 1.f, 1.f};
const auto green = Color {0.f, 1.f, 0.f, 1.f};
} // namespace

auto tSharesByColour = test("GradientRamps/oneRowPerDistinctStopList") = []
{
    if (!hasDevice())
        return;

    auto ramps = GradientRamps {};

    auto first = ramps.rowFor(twoStop(red, blue));
    auto again = ramps.rowFor(twoStop(red, blue));

    check(first >= 0.f);
    check(first == again, "the same colours are the same row");
    check(ramps.getRowCount() == 1);

    auto other = ramps.rowFor(twoStop(red, green));

    check(other != first, "different colours are not");
    check(ramps.getRowCount() == 2);
};

// The axis and the spread are not part of what a row holds, so two gradients
// that differ only in those share one. Which is the case a document hits
// constantly: one set of colours, laid out a dozen ways.
auto tSharesAcrossPlacement =
    test("GradientRamps/theRowIsTheColoursAndNothingElse") = []
{
    if (!hasDevice())
        return;

    auto ramps = GradientRamps {};

    auto plain = twoStop(red, blue);

    auto elsewhere = plain;
    elsewhere.start = {40.f, 90.f};
    elsewhere.end = {200.f, 12.f};
    elsewhere.spread = GradientSpread::Repeat;
    elsewhere.transform = GPUWidgets::AffineTransform::rotation(1.f);

    auto radial = plain;
    radial.kind = Gradient::Kind::Radial;
    radial.radius = 30.f;

    check(ramps.rowFor(plain) == ramps.rowFor(elsewhere));
    check(ramps.rowFor(plain) == ramps.rowFor(radial));
    check(ramps.getRowCount() == 1);
};

// Sorting is the baking's business, so two stop lists that describe the same
// ramp in a different order are still one row.
auto tOrderDoesNotMatter =
    test("GradientRamps/stopsAreNormalisedBeforeTheyAreKeyed") = []
{
    if (!hasDevice())
        return;

    auto ramps = GradientRamps {};

    auto forwards = twoStop(red, blue);

    auto backwards = Gradient {};
    backwards.stops = {{blue, 1.f}, {red, 0.f}};

    check(ramps.rowFor(forwards) == ramps.rowFor(backwards));
    check(ramps.getRowCount() == 1);
};

auto tNoStops = test("GradientRamps/aGradientWithNoStopsTakesNoRow") = []
{
    if (!hasDevice())
        return;

    auto ramps = GradientRamps {};

    check(ramps.rowFor(Gradient {}) < 0.f);
    check(ramps.getRowCount() == 0);
    check(ramps.getDroppedCount() == 0, "nothing was refused; there was nothing");
};

auto tCeiling = test("GradientRamps/pastTheCeilingAGradientIsRefusedAndCounted") = []
{
    if (!hasDevice())
        return;

    auto ramps = GradientRamps {};

    // One distinct stop list per row, so the last one asked for has nowhere to
    // go. The green channel is what makes them differ, and 256 shades of it are
    // 256 different ramps.
    for (auto i = 0; i < GradientRamps::maxRows; ++i)
        check(ramps.rowFor(twoStop(red, {0.f, (float) i / 512.f, 0.f, 1.f})) >= 0.f);

    check(ramps.getRowCount() == GradientRamps::maxRows);
    check(ramps.getDroppedCount() == 0);

    check(ramps.rowFor(twoStop(blue, green)) < 0.f, "and the next one has no room");
    check(ramps.getDroppedCount() == 1);

    // A gradient already baked is still found, which is what stops a full
    // texture from taking the whole document down with it.
    check(ramps.rowFor(twoStop(red, {0.f, 0.f, 0.f, 1.f})) >= 0.f);
};

// Committing twice with nothing in between must not re-upload, and committing
// with nothing at all must not touch a texture that was never made.
auto tCommitIsIdempotent = test("GradientRamps/commitUploadsOnlyWhatIsNew") = []
{
    if (!hasDevice())
        return;

    auto ramps = GradientRamps {};

    ramps.commit();

    ramps.rowFor(twoStop(red, blue));
    ramps.commit();
    ramps.commit();

    ramps.rowFor(twoStop(red, green));
    ramps.commit();

    check(ramps.getRowCount() == 2);
};
