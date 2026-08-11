#include "SVGPathParser.h"
#include "NumberReader.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace eacp::SVG
{

namespace
{
Graphics::Point
    readPoint(NumberReader& reader, bool relative, const Graphics::Point& current)
{
    auto x = reader.readFloat();
    auto y = reader.readFloat();
    if (relative)
        return {current.x + x, current.y + y};
    return {x, y};
}

Graphics::Point smoothControl(const Graphics::Point& current,
                              const Graphics::Point& lastControl)
{
    return {2.f * current.x - lastControl.x, 2.f * current.y - lastControl.y};
}

bool isSmoothCubicContinuation(char lastCommand)
{
    return lastCommand == 'c' || lastCommand == 'C' || lastCommand == 's'
           || lastCommand == 'S';
}

bool isSmoothQuadContinuation(char lastCommand)
{
    return lastCommand == 'q' || lastCommand == 'Q' || lastCommand == 't'
           || lastCommand == 'T';
}

struct PathState
{
    Graphics::Point current {0.f, 0.f};
    Graphics::Point subpathStart {0.f, 0.f};
    Graphics::Point lastControl {0.f, 0.f};
    char lastCommand = 0;
};

template <typename PathType>
void handleMoveTo(NumberReader& reader,
                  PathType& path,
                  PathState& state,
                  bool relative)
{
    auto pt = readPoint(reader, relative, state.current);
    path.moveTo(pt);
    state.current = pt;
    state.subpathStart = pt;
    state.lastCommand = relative ? 'l' : 'L';

    while (reader.hasNumber())
    {
        pt = readPoint(reader, relative, state.current);
        path.lineTo(pt);
        state.current = pt;
    }
}

template <typename PathType>
void handleLineTo(NumberReader& reader,
                  PathType& path,
                  PathState& state,
                  bool relative)
{
    do
    {
        auto pt = readPoint(reader, relative, state.current);
        path.lineTo(pt);
        state.current = pt;
    } while (reader.hasNumber());
}

template <typename PathType>
void handleHorizontalLine(NumberReader& reader,
                          PathType& path,
                          PathState& state,
                          bool relative)
{
    do
    {
        auto x = reader.readFloat();
        if (relative)
            x += state.current.x;
        path.lineTo({x, state.current.y});
        state.current.x = x;
    } while (reader.hasNumber());
}

template <typename PathType>
void handleVerticalLine(NumberReader& reader,
                        PathType& path,
                        PathState& state,
                        bool relative)
{
    do
    {
        auto y = reader.readFloat();
        if (relative)
            y += state.current.y;
        path.lineTo({state.current.x, y});
        state.current.y = y;
    } while (reader.hasNumber());
}

template <typename PathType>
void handleCubic(NumberReader& reader,
                 PathType& path,
                 PathState& state,
                 bool relative)
{
    do
    {
        auto c1 = readPoint(reader, relative, state.current);
        auto c2 = readPoint(reader, relative, state.current);
        auto pt = readPoint(reader, relative, state.current);
        path.cubicTo(c1.x, c1.y, c2.x, c2.y, pt.x, pt.y);
        state.lastControl = c2;
        state.current = pt;
    } while (reader.hasNumber());
}

template <typename PathType>
void handleSmoothCubic(NumberReader& reader,
                       PathType& path,
                       PathState& state,
                       bool relative)
{
    do
    {
        auto c1 = isSmoothCubicContinuation(state.lastCommand)
                      ? smoothControl(state.current, state.lastControl)
                      : state.current;
        auto c2 = readPoint(reader, relative, state.current);
        auto pt = readPoint(reader, relative, state.current);
        path.cubicTo(c1.x, c1.y, c2.x, c2.y, pt.x, pt.y);
        state.lastControl = c2;
        state.current = pt;
    } while (reader.hasNumber());
}

template <typename PathType>
void handleQuadratic(NumberReader& reader,
                     PathType& path,
                     PathState& state,
                     bool relative)
{
    do
    {
        auto ctrl = readPoint(reader, relative, state.current);
        auto pt = readPoint(reader, relative, state.current);
        path.quadTo(ctrl.x, ctrl.y, pt.x, pt.y);
        state.lastControl = ctrl;
        state.current = pt;
    } while (reader.hasNumber());
}

template <typename PathType>
void handleSmoothQuadratic(NumberReader& reader,
                           PathType& path,
                           PathState& state,
                           bool relative)
{
    do
    {
        auto ctrl = isSmoothQuadContinuation(state.lastCommand)
                        ? smoothControl(state.current, state.lastControl)
                        : state.current;
        auto pt = readPoint(reader, relative, state.current);
        path.quadTo(ctrl.x, ctrl.y, pt.x, pt.y);
        state.lastControl = ctrl;
        state.current = pt;
    } while (reader.hasNumber());
}

// An elliptical arc as its centre and the two angles across it, converted from
// the endpoint form a `d` attribute writes. The conversion below is the
// specification's own: F.6.5, with the radius correction of F.6.6.
struct CentredArc
{
    Graphics::Point pointAt(float angle) const
    {
        auto x = radiusX * std::cos(angle);
        auto y = radiusY * std::sin(angle);

        return {centre.x + x * cosRotation - y * sinRotation,
                centre.y + x * sinRotation + y * cosRotation};
    }

    Graphics::Point tangentAt(float angle) const
    {
        auto x = -radiusX * std::sin(angle);
        auto y = radiusY * std::cos(angle);

        return {x * cosRotation - y * sinRotation,
                x * sinRotation + y * cosRotation};
    }

    Graphics::Point centre;
    float radiusX = 0.f;
    float radiusY = 0.f;
    float cosRotation = 1.f;
    float sinRotation = 0.f;
    float startAngle = 0.f;
    float sweepAngle = 0.f;
};

float angleBetween(const Graphics::Point& from, const Graphics::Point& to)
{
    return std::atan2(from.x * to.y - from.y * to.x, from.x * to.x + from.y * to.y);
}

// Nothing where the arc is degenerate -- a zero radius, or an endpoint the
// start is already at -- which the format says to draw as a line or not at all.
std::optional<CentredArc> centreArc(const Graphics::Point& start,
                                    const Graphics::Point& end,
                                    float radiusX,
                                    float radiusY,
                                    float rotationRadians,
                                    bool largeArc,
                                    bool sweep)
{
    if (radiusX == 0.f || radiusY == 0.f)
        return std::nullopt;

    auto arc = CentredArc {};
    arc.radiusX = std::abs(radiusX);
    arc.radiusY = std::abs(radiusY);
    arc.cosRotation = std::cos(rotationRadians);
    arc.sinRotation = std::sin(rotationRadians);

    // The endpoints in the ellipse's own frame, with the chord between them
    // centred on the origin.
    auto halfChordX = (start.x - end.x) * 0.5f;
    auto halfChordY = (start.y - end.y) * 0.5f;

    auto x1 = arc.cosRotation * halfChordX + arc.sinRotation * halfChordY;
    auto y1 = -arc.sinRotation * halfChordX + arc.cosRotation * halfChordY;

    if (x1 == 0.f && y1 == 0.f)
        return std::nullopt;

    // Radii too small to reach across the chord are grown until they just do,
    // rather than the arc being dropped.
    auto overshoot = (x1 * x1) / (arc.radiusX * arc.radiusX)
                     + (y1 * y1) / (arc.radiusY * arc.radiusY);

    if (overshoot > 1.f)
    {
        auto growth = std::sqrt(overshoot);
        arc.radiusX *= growth;
        arc.radiusY *= growth;
    }

    auto rxSquared = arc.radiusX * arc.radiusX;
    auto rySquared = arc.radiusY * arc.radiusY;

    auto denominator = rxSquared * y1 * y1 + rySquared * x1 * x1;
    auto numerator = rxSquared * rySquared - denominator;

    // The two flags pick one of the four arcs any pair of points and radii
    // admit: which circle through the endpoints, and which way round it.
    auto distance = std::sqrt(std::max(0.f, numerator / denominator))
                    * (largeArc != sweep ? 1.f : -1.f);

    auto centreX = distance * arc.radiusX * y1 / arc.radiusY;
    auto centreY = -distance * arc.radiusY * x1 / arc.radiusX;

    arc.centre = {arc.cosRotation * centreX - arc.sinRotation * centreY
                      + (start.x + end.x) * 0.5f,
                  arc.sinRotation * centreX + arc.cosRotation * centreY
                      + (start.y + end.y) * 0.5f};

    auto toStart =
        Graphics::Point {(x1 - centreX) / arc.radiusX, (y1 - centreY) / arc.radiusY};
    auto toEnd = Graphics::Point {(-x1 - centreX) / arc.radiusX,
                                  (-y1 - centreY) / arc.radiusY};

    arc.startAngle = angleBetween({1.f, 0.f}, toStart);
    arc.sweepAngle = angleBetween(toStart, toEnd);

    // atan2 gives the sweep in (-pi, pi], and the flag says which way round the
    // ellipse the arc actually goes, so one of the two needs a whole turn added.
    if (!sweep && arc.sweepAngle > 0.f)
        arc.sweepAngle -= 2.f * GPUWidgets::pi;
    else if (sweep && arc.sweepAngle < 0.f)
        arc.sweepAngle += 2.f * GPUWidgets::pi;

    return arc;
}

// The arc as cubics, neither path type having an arc call. A quarter turn at a
// time, where a cubic matching the ellipse's ends and tangents is within about
// a ten-thousandth of the radius.
template <typename PathType>
void addArcSegments(PathType& path, const CentredArc& arc)
{
    auto quarters = std::max(
        1, (int) std::ceil(std::abs(arc.sweepAngle) / (GPUWidgets::pi * 0.5f)));

    auto step = arc.sweepAngle / (float) quarters;

    // The fraction of the tangent that puts the control points where the cubic
    // passes through the arc's midpoint as well as its ends.
    auto reach = 4.f / 3.f * std::tan(step * 0.25f);

    for (auto i = 0; i < quarters; ++i)
    {
        auto from = arc.startAngle + step * (float) i;
        auto to = from + step;

        auto start = arc.pointAt(from);
        auto end = arc.pointAt(to);
        auto startTangent = arc.tangentAt(from);
        auto endTangent = arc.tangentAt(to);

        path.cubicTo(start.x + startTangent.x * reach,
                     start.y + startTangent.y * reach,
                     end.x - endTangent.x * reach,
                     end.y - endTangent.y * reach,
                     end.x,
                     end.y);
    }
}

template <typename PathType>
void handleArc(NumberReader& reader, PathType& path, PathState& state, bool relative)
{
    do
    {
        auto radiusX = reader.readFloat();
        auto radiusY = reader.readFloat();
        auto rotation = reader.readFloat() * GPUWidgets::pi / 180.f;

        // Flags rather than numbers, and the difference matters -- see
        // NumberReader::readFlag.
        auto largeArc = reader.readFlag();
        auto sweep = reader.readFlag();

        auto end = readPoint(reader, relative, state.current);

        auto arc = centreArc(
            state.current, end, radiusX, radiusY, rotation, largeArc, sweep);

        // A degenerate arc is the straight line to its endpoint, per the format.
        if (arc.has_value())
            addArcSegments(path, *arc);
        else
            path.lineTo(end);

        state.current = end;
    } while (reader.hasNumber());
}

template <typename PathType>
void handleClosePath(PathType& path, PathState& state)
{
    path.close();
    state.current = state.subpathStart;
}

char readCommandChar(NumberReader& reader, char lastCommand)
{
    auto c = reader.src[reader.pos];
    if (std::isalpha(static_cast<unsigned char>(c)))
    {
        reader.pos++;
        return c;
    }
    return lastCommand;
}

template <typename PathType>
void dispatchCommand(char cmd,
                     NumberReader& reader,
                     PathType& path,
                     PathState& state)
{
    auto relative = std::islower(static_cast<unsigned char>(cmd));
    auto upper = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));

    switch (upper)
    {
        case 'M':
            handleMoveTo(reader, path, state, relative);
            break;
        case 'L':
            handleLineTo(reader, path, state, relative);
            break;
        case 'H':
            handleHorizontalLine(reader, path, state, relative);
            break;
        case 'V':
            handleVerticalLine(reader, path, state, relative);
            break;
        case 'C':
            handleCubic(reader, path, state, relative);
            break;
        case 'S':
            handleSmoothCubic(reader, path, state, relative);
            break;
        case 'Q':
            handleQuadratic(reader, path, state, relative);
            break;
        case 'T':
            handleSmoothQuadratic(reader, path, state, relative);
            break;
        case 'A':
            handleArc(reader, path, state, relative);
            break;
        case 'Z':
            handleClosePath(path, state);
            break;
        default:
            reader.pos++;
            return;
    }

    if (upper != 'M')
        state.lastCommand = cmd;
}
} // namespace

template <typename PathType>
void parseSVGPathInto(const std::string& d, PathType& path)
{
    auto reader = NumberReader {d, 0};
    auto state = PathState();

    while (!reader.atEnd())
    {
        reader.skipWhitespaceAndCommas();
        if (reader.atEnd())
            break;

        auto cmd = readCommandChar(reader, state.lastCommand);
        dispatchCommand(cmd, reader, path, state);
    }
}

template void parseSVGPathInto<Graphics::Path>(const std::string&, Graphics::Path&);
template void parseSVGPathInto<GPUWidgets::Path>(const std::string&,
                                                 GPUWidgets::Path&);

} // namespace eacp::SVG
