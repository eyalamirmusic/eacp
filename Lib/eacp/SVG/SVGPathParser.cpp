#include "SVGPathParser.h"
#include <eacp/Core/Utils/Logging.h>
#include <cctype>
#include <cmath>

namespace eacp::SVG
{

struct PathReader
{
    const std::string& src;
    size_t pos = 0;

    bool atEnd() const { return pos >= src.size(); }

    void skipWhitespaceAndCommas()
    {
        while (!atEnd()
               && (std::isspace(static_cast<unsigned char>(src[pos]))
                   || src[pos] == ','))
        {
            ++pos;
        }
    }

    bool hasNumber()
    {
        skipWhitespaceAndCommas();
        if (atEnd())
            return false;
        auto c = src[pos];
        return std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+'
               || c == '.';
    }

    float readNumber()
    {
        skipWhitespaceAndCommas();
        auto start = pos;

        if (!atEnd() && (src[pos] == '-' || src[pos] == '+'))
            ++pos;

        while (!atEnd() && std::isdigit(static_cast<unsigned char>(src[pos])))
        {
            ++pos;
        }

        if (!atEnd() && src[pos] == '.')
        {
            ++pos;
            while (!atEnd() && std::isdigit(static_cast<unsigned char>(src[pos])))
            {
                ++pos;
            }
        }

        if (!atEnd() && (src[pos] == 'e' || src[pos] == 'E'))
        {
            ++pos;
            if (!atEnd() && (src[pos] == '-' || src[pos] == '+'))
                ++pos;
            while (!atEnd() && std::isdigit(static_cast<unsigned char>(src[pos])))
            {
                ++pos;
            }
        }

        if (pos == start)
            return 0.f;

        return std::stof(src.substr(start, pos - start));
    }

    Graphics::Point readPoint(bool relative, const Graphics::Point& current)
    {
        auto x = readNumber();
        auto y = readNumber();
        if (relative)
            return {current.x + x, current.y + y};
        return {x, y};
    }
};

Graphics::Path parseSVGPath(const std::string& d)
{
    Graphics::Path path;
    PathReader reader {d};

    Graphics::Point current {0.f, 0.f};
    Graphics::Point subpathStart {0.f, 0.f};
    Graphics::Point lastControl {0.f, 0.f};
    char lastCommand = 0;

    while (!reader.atEnd())
    {
        reader.skipWhitespaceAndCommas();
        if (reader.atEnd())
            break;

        auto c = reader.src[reader.pos];
        auto isCommand = std::isalpha(static_cast<unsigned char>(c));

        char cmd;
        if (isCommand)
        {
            cmd = c;
            reader.pos++;
        }
        else
        {
            cmd = lastCommand;
        }

        auto relative = std::islower(static_cast<unsigned char>(cmd));
        auto upper =
            static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));

        switch (upper)
        {
            case 'M':
            {
                auto pt = reader.readPoint(relative, current);
                path.moveTo(pt);
                current = pt;
                subpathStart = pt;
                lastCommand = relative ? 'l' : 'L';
                while (reader.hasNumber())
                {
                    pt = reader.readPoint(relative, current);
                    path.lineTo(pt);
                    current = pt;
                }
                break;
            }
            case 'L':
            {
                do
                {
                    auto pt = reader.readPoint(relative, current);
                    path.lineTo(pt);
                    current = pt;
                } while (reader.hasNumber());
                lastCommand = cmd;
                break;
            }
            case 'H':
            {
                do
                {
                    auto x = reader.readNumber();
                    if (relative)
                        x += current.x;
                    path.lineTo({x, current.y});
                    current.x = x;
                } while (reader.hasNumber());
                lastCommand = cmd;
                break;
            }
            case 'V':
            {
                do
                {
                    auto y = reader.readNumber();
                    if (relative)
                        y += current.y;
                    path.lineTo({current.x, y});
                    current.y = y;
                } while (reader.hasNumber());
                lastCommand = cmd;
                break;
            }
            case 'C':
            {
                do
                {
                    auto c1 = reader.readPoint(relative, current);
                    auto c2 = reader.readPoint(relative, current);
                    auto pt = reader.readPoint(relative, current);
                    path.cubicTo(c1.x, c1.y, c2.x, c2.y, pt.x, pt.y);
                    lastControl = c2;
                    current = pt;
                } while (reader.hasNumber());
                lastCommand = cmd;
                break;
            }
            case 'S':
            {
                do
                {
                    Graphics::Point c1;
                    if (lastCommand == 'c' || lastCommand == 'C'
                        || lastCommand == 's' || lastCommand == 'S')
                    {
                        c1 = {2.f * current.x - lastControl.x,
                              2.f * current.y - lastControl.y};
                    }
                    else
                    {
                        c1 = current;
                    }
                    auto c2 = reader.readPoint(relative, current);
                    auto pt = reader.readPoint(relative, current);
                    path.cubicTo(c1.x, c1.y, c2.x, c2.y, pt.x, pt.y);
                    lastControl = c2;
                    current = pt;
                } while (reader.hasNumber());
                lastCommand = cmd;
                break;
            }
            case 'Q':
            {
                do
                {
                    auto ctrl = reader.readPoint(relative, current);
                    auto pt = reader.readPoint(relative, current);
                    path.quadTo(ctrl.x, ctrl.y, pt.x, pt.y);
                    lastControl = ctrl;
                    current = pt;
                } while (reader.hasNumber());
                lastCommand = cmd;
                break;
            }
            case 'T':
            {
                do
                {
                    Graphics::Point ctrl;
                    if (lastCommand == 'q' || lastCommand == 'Q'
                        || lastCommand == 't' || lastCommand == 'T')
                    {
                        ctrl = {2.f * current.x - lastControl.x,
                                2.f * current.y - lastControl.y};
                    }
                    else
                    {
                        ctrl = current;
                    }
                    auto pt = reader.readPoint(relative, current);
                    path.quadTo(ctrl.x, ctrl.y, pt.x, pt.y);
                    lastControl = ctrl;
                    current = pt;
                } while (reader.hasNumber());
                lastCommand = cmd;
                break;
            }
            case 'A':
            {
                LOG("SVG: Arc commands (A/a) not yet supported");
                while (reader.hasNumber())
                {
                    for (auto i = 0; i < 7 && reader.hasNumber(); ++i)
                    {
                        reader.readNumber();
                    }
                }
                lastCommand = cmd;
                break;
            }
            case 'Z':
            {
                path.close();
                current = subpathStart;
                lastCommand = cmd;
                break;
            }
            default:
                reader.pos++;
                break;
        }
    }

    return path;
}

} // namespace eacp::SVG
