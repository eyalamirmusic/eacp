#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

#include <string>

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
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

    // The green channel is what makes each stop list distinct.
    for (auto i = 0; i < GradientRamps::maxRows; ++i)
        check(ramps.rowFor(twoStop(red, {0.f, (float) i / 512.f, 0.f, 1.f})) >= 0.f);

    check(ramps.getRowCount() == GradientRamps::maxRows);
    check(ramps.getDroppedCount() == 0);

    check(ramps.rowFor(twoStop(blue, green)) < 0.f, "and the next one has no room");
    check(ramps.getDroppedCount() == 1);

    check(ramps.rowFor(twoStop(red, {0.f, 0.f, 0.f, 1.f})) >= 0.f);
};

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
