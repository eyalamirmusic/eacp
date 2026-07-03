#include "SVGPathParser.h"
#include "NumberReader.h"

#include <algorithm>
#include <cctype>
#include <cmath>

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

void handleMoveTo(NumberReader& reader,
                  Graphics::Path& path,
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

void handleLineTo(NumberReader& reader,
                  Graphics::Path& path,
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

void handleHorizontalLine(NumberReader& reader,
                          Graphics::Path& path,
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

void handleVerticalLine(NumberReader& reader,
                        Graphics::Path& path,
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

void handleCubic(NumberReader& reader,
                 Graphics::Path& path,
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

void handleSmoothCubic(NumberReader& reader,
                       Graphics::Path& path,
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

void handleQuadratic(NumberReader& reader,
                     Graphics::Path& path,
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

void handleSmoothQuadratic(NumberReader& reader,
                           Graphics::Path& path,
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

// Arc flags are single digits that the grammar allows to run straight into
// the next number ("...0 01.5 2"), so they cannot be read as floats.
bool readArcFlag(NumberReader& reader)
{
    reader.skipWhitespaceAndCommas();
    if (reader.atEnd())
        return false;

    auto c = reader.peek();
    if (c == '0' || c == '1')
    {
        reader.pos++;
        return c == '1';
    }
    return reader.readFloat() != 0.f;
}

// Approximates one elliptical-arc slice of `step` radians (at most a
// quarter turn) with a single cubic, using the standard tangent-scaled
// control points.
void emitArcSegment(Graphics::Path& path,
                    const Graphics::Point& center,
                    float rx,
                    float ry,
                    float cosPhi,
                    float sinPhi,
                    float theta,
                    float step)
{
    auto pointAt = [&](float t)
    {
        auto ex = rx * std::cos(t);
        auto ey = ry * std::sin(t);
        return Graphics::Point {center.x + ex * cosPhi - ey * sinPhi,
                                center.y + ex * sinPhi + ey * cosPhi};
    };
    auto derivativeAt = [&](float t)
    {
        auto ex = -rx * std::sin(t);
        auto ey = ry * std::cos(t);
        return Graphics::Point {ex * cosPhi - ey * sinPhi,
                                ex * sinPhi + ey * cosPhi};
    };

    auto k = 4.f / 3.f * std::tan(step / 4.f);
    auto from = pointAt(theta);
    auto to = pointAt(theta + step);
    auto d1 = derivativeAt(theta);
    auto d2 = derivativeAt(theta + step);

    path.cubicTo(from.x + k * d1.x,
                 from.y + k * d1.y,
                 to.x - k * d2.x,
                 to.y - k * d2.y,
                 to.x,
                 to.y);
}

// SVG spec appendix B.2.4: convert the endpoint parameterization to the
// centre form, then emit one cubic per quarter turn.
void emitArc(Graphics::Path& path,
             const Graphics::Point& start,
             const Graphics::Point& end,
             float rx,
             float ry,
             float rotationDegrees,
             bool largeArc,
             bool sweep)
{
    if (start.x == end.x && start.y == end.y)
        return;

    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx == 0.f || ry == 0.f)
    {
        path.lineTo(end);
        return;
    }

    constexpr auto pi = 3.14159265358979323846f;
    auto phi = rotationDegrees * pi / 180.f;
    auto cosPhi = std::cos(phi);
    auto sinPhi = std::sin(phi);

    auto dx = (start.x - end.x) / 2.f;
    auto dy = (start.y - end.y) / 2.f;
    auto x1 = cosPhi * dx + sinPhi * dy;
    auto y1 = -sinPhi * dx + cosPhi * dy;

    // Radii too small to span the endpoints get scaled up uniformly.
    auto lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1.f)
    {
        auto scale = std::sqrt(lambda);
        rx *= scale;
        ry *= scale;
    }

    auto numerator = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    auto denominator = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    auto factor = std::sqrt(std::max(0.f, numerator / denominator));
    if (largeArc == sweep)
        factor = -factor;

    auto cx1 = factor * rx * y1 / ry;
    auto cy1 = -factor * ry * x1 / rx;
    auto center =
        Graphics::Point {cosPhi * cx1 - sinPhi * cy1 + (start.x + end.x) / 2.f,
                         sinPhi * cx1 + cosPhi * cy1 + (start.y + end.y) / 2.f};

    auto theta1 = std::atan2((y1 - cy1) / ry, (x1 - cx1) / rx);
    auto theta2 = std::atan2((-y1 - cy1) / ry, (-x1 - cx1) / rx);
    auto delta = theta2 - theta1;
    if (!sweep && delta > 0.f)
        delta -= 2.f * pi;
    if (sweep && delta < 0.f)
        delta += 2.f * pi;

    auto segments =
        std::max(1, static_cast<int>(std::ceil(std::abs(delta) / (pi / 2.f))));
    auto step = delta / static_cast<float>(segments);

    for (auto i = 0; i < segments; ++i)
        emitArcSegment(path,
                       center,
                       rx,
                       ry,
                       cosPhi,
                       sinPhi,
                       theta1 + step * static_cast<float>(i),
                       step);
}

void handleArc(NumberReader& reader,
               Graphics::Path& path,
               PathState& state,
               bool relative)
{
    do
    {
        auto rx = reader.readFloat();
        auto ry = reader.readFloat();
        auto rotation = reader.readFloat();
        auto largeArc = readArcFlag(reader);
        auto sweep = readArcFlag(reader);
        auto end = readPoint(reader, relative, state.current);

        emitArc(path, state.current, end, rx, ry, rotation, largeArc, sweep);
        state.current = end;
    } while (reader.hasNumber());
}

void handleClosePath(Graphics::Path& path, PathState& state)
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

void dispatchCommand(char cmd,
                     NumberReader& reader,
                     Graphics::Path& path,
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

Graphics::Path parseSVGPath(const std::string& d)
{
    auto path = Graphics::Path();
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

    return path;
}

} // namespace eacp::SVG
