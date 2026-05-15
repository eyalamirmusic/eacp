#include "SVGPathParser.h"
#include "NumberReader.h"

#include <eacp/Core/Utils/Logging.h>

#include <cctype>

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

void handleArc(NumberReader& reader)
{
    LOG("SVG: Arc commands (A/a) not yet supported");
    while (reader.hasNumber())
    {
        for (auto i = 0; i < 7 && reader.hasNumber(); ++i)
            reader.readFloat();
    }
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
            handleArc(reader);
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
