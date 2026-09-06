#include "Common.h"

#include <eacp/Graphics/Primitives/Path-Linux.h>

#include <cmath>

// A Linux Path is the only one whose contents can be read back: Apple and
// Windows hand their points to CGPath and ID2D1PathGeometry, which do not give
// them out again, so those two are only ever checked by rendering. Here the
// record is the whole implementation, and the tessellator that will consume it
// is not written yet — so these pin the transcript itself. A curve emitted with
// its control points the wrong way round draws a shape that is still closed,
// still inside its rect and still plausible, which is exactly the kind of bug
// that survives until someone looks at a screenshot.

using namespace nano;
using eacp::Vector;
using eacp::Graphics::getPathGeometry;
using eacp::Graphics::Path;
using eacp::Graphics::PathCommand;
using eacp::Graphics::Point;
using eacp::Graphics::Rect;

namespace
{
using Verb = PathCommand::Verb;

bool isClose(float a, float b)
{
    return std::abs(a - b) < 0.001f;
}

bool isAt(const Point& point, float x, float y)
{
    return isClose(point.x, x) && isClose(point.y, y);
}

bool isWithin(const Point& point, const Rect& rect)
{
    return point.x >= rect.left() - 0.001f && point.x <= rect.right() + 0.001f
           && point.y >= rect.top() - 0.001f && point.y <= rect.bottom() + 0.001f;
}

// Where the pen lands after a command: the last point the verb uses.
Point endPointOf(const PathCommand& command)
{
    return command.points[command.pointCount() - 1];
}

Vector<Verb> verbsOf(const Path& path)
{
    auto verbs = Vector<Verb>();

    for (const auto& command: getPathGeometry(path).commands)
        verbs.add(command.verb);

    return verbs;
}
} // namespace

auto tPathStartsEmpty = test("Path/newPathRecordsNothing") = []
{
    auto path = Path();

    check(getPathGeometry(path).commands.empty());
    check(path.getHandle() == &getPathGeometry(path));
};

auto tPathRecordsEachBuilderCall = test("Path/recordsOneCommandPerCall") = []
{
    auto path = Path();
    path.moveTo({1.f, 2.f});
    path.lineTo({3.f, 4.f});
    path.quadTo(5.f, 6.f, 7.f, 8.f);
    path.cubicTo(9.f, 10.f, 11.f, 12.f, 13.f, 14.f);
    path.close();

    const auto& commands = getPathGeometry(path).commands;
    check(commands.size() == 5);

    check(commands[0].verb == Verb::move);
    check(commands[0].pointCount() == 1);
    check(isAt(commands[0].points[0], 1.f, 2.f));

    check(commands[1].verb == Verb::line);
    check(commands[1].pointCount() == 1);
    check(isAt(commands[1].points[0], 3.f, 4.f));

    check(commands[2].verb == Verb::quad);
    check(commands[2].pointCount() == 2);
    check(isAt(commands[2].points[0], 5.f, 6.f));
    check(isAt(commands[2].points[1], 7.f, 8.f));

    check(commands[3].verb == Verb::cubic);
    check(commands[3].pointCount() == 3);
    check(isAt(commands[3].points[0], 9.f, 10.f));
    check(isAt(commands[3].points[1], 11.f, 12.f));
    check(isAt(commands[3].points[2], 13.f, 14.f));

    check(commands[4].verb == Verb::close);
    check(commands[4].pointCount() == 0);
};

auto tPathRectIsFourCorners = test("Path/addRectWalksTheFourCorners") = []
{
    auto path = Path();
    path.addRect({10.f, 20.f, 30.f, 40.f});

    check(verbsOf(path)
          == Vector<Verb> {
              Verb::move, Verb::line, Verb::line, Verb::line, Verb::close});

    const auto& commands = getPathGeometry(path).commands;
    check(isAt(commands[0].points[0], 10.f, 20.f));
    check(isAt(commands[1].points[0], 40.f, 20.f));
    check(isAt(commands[2].points[0], 40.f, 60.f));
    check(isAt(commands[3].points[0], 10.f, 60.f));
};

// Four corners, each an edge then a cubic, then the closing edge back to the
// start. The last cubic's endpoint has to be the moveTo's point or the close
// draws a chord across the corner.
auto tPathRoundedRectShape = test("Path/addRoundedRectIsEdgesAndCorners") = []
{
    auto path = Path();
    path.addRoundedRect({0.f, 0.f, 100.f, 60.f}, 10.f);

    check(verbsOf(path)
          == Vector<Verb> {Verb::move,
                           Verb::line,
                           Verb::cubic,
                           Verb::line,
                           Verb::cubic,
                           Verb::line,
                           Verb::cubic,
                           Verb::line,
                           Verb::cubic,
                           Verb::close});

    const auto& commands = getPathGeometry(path).commands;
    check(isAt(commands[0].points[0], 10.f, 0.f));
    check(isAt(endPointOf(commands[2]), 100.f, 10.f));
    check(isAt(endPointOf(commands[4]), 90.f, 60.f));
    check(isAt(endPointOf(commands[6]), 0.f, 50.f));
    check(isAt(endPointOf(commands[8]), 10.f, 0.f));
};

// The radius has to be fitted before the corners are laid out, or the top edge
// runs backwards: at r = 30 on a 4pt-tall rect the "top right" corner starts
// left of where the "top left" one ended.
auto tPathRoundedRectClampsRadius = test("Path/addRoundedRectClampsTheRadius") = []
{
    auto bar = Rect {24.f, 80.f, 472.f, 4.f};

    auto path = Path();
    path.addRoundedRect(bar, 30.f);

    const auto& commands = getPathGeometry(path).commands;
    check(isAt(commands[0].points[0], 26.f, 80.f));
    check(isAt(commands[1].points[0], 494.f, 80.f));
    check(isAt(endPointOf(commands[2]), 496.f, 82.f));

    for (const auto& command: commands)
        for (auto i = 0; i < command.pointCount(); ++i)
            check(isWithin(command.points[i], bar));
};

// A degenerate rect keeps its corners at zero rather than producing a radius it
// cannot fit; the shape collapses to a line, which is what every backend draws.
auto tPathRoundedRectOfAnEmptyRect = test("Path/addRoundedRectOfAnEmptyRect") = []
{
    auto path = Path();
    path.addRoundedRect({5.f, 5.f, 0.f, 20.f}, 4.f);

    const auto& commands = getPathGeometry(path).commands;
    check(commands.size() == 10);
    check(isAt(commands[0].points[0], 5.f, 5.f));
    check(isAt(endPointOf(commands[8]), 5.f, 5.f));
};

auto tPathEllipseIsFourClosedArcs = test("Path/addEllipseIsFourArcsAndAClose") = []
{
    auto path = Path();
    path.addEllipse({0.f, 0.f, 80.f, 40.f});

    check(verbsOf(path)
          == Vector<Verb> {Verb::move,
                           Verb::cubic,
                           Verb::cubic,
                           Verb::cubic,
                           Verb::cubic,
                           Verb::close});

    const auto& commands = getPathGeometry(path).commands;
    check(isAt(commands[0].points[0], 80.f, 20.f));
    check(isAt(endPointOf(commands[1]), 40.f, 40.f));
    check(isAt(endPointOf(commands[2]), 0.f, 20.f));
    check(isAt(endPointOf(commands[3]), 40.f, 0.f));
    check(isAt(endPointOf(commands[4]), 80.f, 20.f));
};

auto tPathScaledMovesEveryPoint = test("Path/scaledScalesEveryPoint") = []
{
    auto path = Path();
    path.moveTo({1.f, 2.f});
    path.cubicTo(3.f, 4.f, 5.f, 6.f, 7.f, 8.f);
    path.close();

    auto scaled = path.scaled(2.f, 10.f);

    check(verbsOf(scaled) == verbsOf(path));

    const auto& commands = getPathGeometry(scaled).commands;
    check(isAt(commands[0].points[0], 2.f, 20.f));
    check(isAt(commands[1].points[0], 6.f, 40.f));
    check(isAt(commands[1].points[1], 10.f, 60.f));
    check(isAt(commands[1].points[2], 14.f, 80.f));

    // The source is a separate record, not a view onto the same one.
    check(isAt(getPathGeometry(path).commands[0].points[0], 1.f, 2.f));
};

auto tPathClearEmptiesTheRecord = test("Path/clearEmptiesTheRecord") = []
{
    auto path = Path();
    path.addEllipse({0.f, 0.f, 10.f, 10.f});
    check(!getPathGeometry(path).commands.empty());

    path.clear();
    check(getPathGeometry(path).commands.empty());

    // Still usable afterwards, and still the same record.
    path.moveTo({1.f, 1.f});
    check(getPathGeometry(path).commands.size() == 1);
    check(path.getHandle() == &getPathGeometry(path));
};
