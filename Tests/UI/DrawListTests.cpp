#include <eacp/UI/UI.h>

#include <NanoTest/NanoTest.h>

// What a paint() leaves behind, and the rule that makes keeping it worthwhile.
//
// The rule is that a list is recorded in the painting component's *own* points
// and knows nothing about where that component sits. Everything follows from it:
// a component that moves replays what it has, a scrolled list costs no paints at
// all, and the frame is free to draw the same recording at two positions. Break
// it -- record one coordinate in surface space -- and the failure is a component
// that draws in the right place until the first time it is moved.

using namespace nano;
using namespace eacp;
using namespace eacp::UI;

namespace
{
// The ramps own a texture and the glyph atlas owns two, so a painter needs a
// device -- but no window, no pass and no drawable. Recording touches none of
// them, which is the point being tested.
bool hasDevice()
{
    return GPU::Device::shared().isValid();
}

// A painter with nowhere to draw, which is all a recording needs.
struct Recorder
{
    explicit Recorder(const Rect& bounds = {0.f, 0.f, 100.f, 50.f})
        : g(list, ramps, text, bounds)
    {
    }

    DrawList list;
    GradientRamps ramps;
    Text::TextRenderer text;
    UI::Graphics g;
};

const auto red = Color {1.f, 0.f, 0.f, 1.f};
const auto blue = Color {0.f, 0.f, 1.f, 1.f};
} // namespace

auto tShapesRecordInOrder = test("DrawList/aPaintIsKeptAsWhatItIssued") = []
{
    if (!hasDevice())
        return;

    auto recorder = Recorder {};

    recorder.g.setColour(red);
    recorder.g.fillRoundedRect({10.f, 20.f, 30.f, 40.f}, 5.f);

    const auto& shapes = recorder.list.getShapes();

    check(shapes.size() == 1);
    check(shapes[0].kind == ShapeDraw::Kind::Fill);
    check(sameRect(shapes[0].rect, {10.f, 20.f, 30.f, 40.f}), "in its own points");
    check(shapes[0].cornerRadius == 5.f);
    check(shapes[0].colour.r == red.r);
};

auto tRunsMerge = test("DrawList/aRunOfShapesIsOneCommand") = []
{
    if (!hasDevice())
        return;

    auto recorder = Recorder {};

    for (auto i = 0; i < 5; ++i)
        recorder.g.fillRect({(float) i * 10.f, 0.f, 8.f, 8.f});

    check(recorder.list.getShapes().size() == 5);

    // Which is what makes replaying a component a walk over a handful of
    // commands rather than over its primitives.
    check(recorder.list.getCommands().size() == 1);
    check(recorder.list.getCommands()[0].count == 5);
};

auto tTranslationIsRecorded =
    test("DrawList/aTranslateInsideAPaintIsPartOfTheShape") = []
{
    if (!hasDevice())
        return;

    auto recorder = Recorder {};

    recorder.g.translate(7.f, 11.f);
    recorder.g.fillRect({1.f, 2.f, 3.f, 4.f});

    // The painter's own origin is resolved at the call, so what is kept is where
    // the shape sits in the component. Only the component's own position is left
    // for the frame to apply.
    check(sameRect(recorder.list.getShapes()[0].rect, {8.f, 13.f, 3.f, 4.f}));
};

auto tClipIsRecordedLazily =
    test("DrawList/aClipIsRecordedWhenSomethingIsDrawnUnderIt") = []
{
    if (!hasDevice())
        return;

    auto recorder = Recorder {};

    {
        auto scope = UI::Graphics::ScopedState {recorder.g};
        recorder.g.reduceClipRegion({0.f, 0.f, 10.f, 10.f});

        // Nothing drawn under it, so there is nothing for it to have cut.
    }

    check(recorder.list.getClips().empty(), "a clip nothing was drawn under");

    recorder.g.reduceClipRegion({0.f, 0.f, 20.f, 20.f});
    recorder.g.fillRect({0.f, 0.f, 50.f, 50.f});

    check(recorder.list.getClips().size() == 1);
    check(sameRect(recorder.list.getClips()[0].region, {0.f, 0.f, 20.f, 20.f}));
};

auto tClipNarrowsOnly = test("DrawList/aClipCannotBeWidenedByAskingg") = []
{
    if (!hasDevice())
        return;

    auto recorder = Recorder {{0.f, 0.f, 100.f, 50.f}};

    recorder.g.reduceClipRegion({0.f, 0.f, 40.f, 40.f});
    recorder.g.reduceClipRegion({0.f, 0.f, 400.f, 400.f});
    recorder.g.fillRect({0.f, 0.f, 100.f, 100.f});

    check(sameRect(recorder.list.getClips()[0].region, {0.f, 0.f, 40.f, 40.f}));
};

auto tTextIsLaidOutOnce =
    test("DrawList/aStringIsLaidOutWhenItIsPaintedAndNotWhenItIsDrawn") = []
{
    if (!hasDevice())
        return;

    auto recorder = Recorder {};

    recorder.text.setViewport({100.f, 50.f}, 1.f);
    recorder.g.setColour(blue);

    auto advance = recorder.g.drawText("hello", {0.f, 20.f});

    check(advance > 0.f, "and it still reports the advance it always did");

    const auto& runs = recorder.list.getGlyphRuns();

    check(runs.size() == 1);
    check(runs[0].count == 5, "one glyph apiece, kept rather than laid out again");
    check(runs[0].colour.b == blue.b);

    // In the component's own points, like everything else: the pen was placed at
    // the baseline and the glyphs sit around it.
    check(recorder.list.getGlyphs()[0].destination.x >= 0.f);
};

auto tGradientIsLocal = test("DrawList/aGradientIsResolvedIntoTheListsOwnSpace") = []
{
    if (!hasDevice())
        return;

    // The same gradient, placed at the same point of two paintings whose origins
    // differ. The recorded map takes a point of the list's own space, so both
    // have to send their own local (10, 0) to the same place along the ramp --
    // which is what lets the frame compose a component's position into it and
    // get the same picture wherever the component has moved to.
    auto plain = Recorder {};
    auto shifted = Recorder {};

    auto gradient = Gradient {};
    gradient.start = {0.f, 0.f};
    gradient.end = {20.f, 0.f};
    gradient.stops = {{red, 0.f}, {blue, 1.f}};

    plain.g.setGradient(gradient);
    plain.g.fillRect({0.f, 0.f, 20.f, 10.f});

    shifted.g.translate(30.f, 0.f);
    shifted.g.setGradient(gradient);
    shifted.g.fillRect({0.f, 0.f, 20.f, 10.f});

    const auto& first = plain.list.getShapes()[0];
    const auto& second = shifted.list.getShapes()[0];

    auto atPlain = first.gradient.toGradientSpace.apply({10.f, 0.f});
    auto atShifted = second.gradient.toGradientSpace.apply({40.f, 0.f});

    check(std::abs(atPlain.x - 0.5f) < 1e-5f, "halfway along the ramp");
    check(std::abs(atShifted.x - atPlain.x) < 1e-5f, "and so is the shifted one");
};

auto tClearKeepsStorage = test("DrawList/rerecordingAllocatesNothing") = []
{
    if (!hasDevice())
        return;

    auto list = DrawList {};
    auto ramps = GradientRamps {};
    auto text = Text::TextRenderer {};

    {
        auto g = UI::Graphics {list, ramps, text, {0.f, 0.f, 10.f, 10.f}};
        g.fillRect({0.f, 0.f, 5.f, 5.f});
    }

    check(!list.isEmpty());

    list.clear();

    check(list.isEmpty(), "and empty is what the next paint writes over");
    check(list.getShapes().empty());
    check(list.getCommands().empty());
};
