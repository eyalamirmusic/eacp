#include "SVGAttributes.h"
#include "NumberReader.h"

#include <eacp/Core/Utils/Strings.h>

#include <cctype>
#include <sstream>
#include <unordered_map>

namespace eacp::SVG
{

static int hexNibble(char c)
{
    auto value = Strings::hexCharToInt(c);
    return value < 0 ? 0 : value;
}

static Graphics::Color parseHexColor(const std::string& hex)
{
    if (hex.size() == 3)
    {
        auto r = hexNibble(hex[0]);
        auto g = hexNibble(hex[1]);
        auto b = hexNibble(hex[2]);
        return {(r * 17) / 255.f, (g * 17) / 255.f, (b * 17) / 255.f};
    }
    if (hex.size() == 6)
    {
        auto r = hexNibble(hex[0]) * 16 + hexNibble(hex[1]);
        auto g = hexNibble(hex[2]) * 16 + hexNibble(hex[3]);
        auto b = hexNibble(hex[4]) * 16 + hexNibble(hex[5]);
        return {r / 255.f, g / 255.f, b / 255.f};
    }
    if (hex.size() == 8)
    {
        auto r = hexNibble(hex[0]) * 16 + hexNibble(hex[1]);
        auto g = hexNibble(hex[2]) * 16 + hexNibble(hex[3]);
        auto b = hexNibble(hex[4]) * 16 + hexNibble(hex[5]);
        auto a = hexNibble(hex[6]) * 16 + hexNibble(hex[7]);
        return {r / 255.f, g / 255.f, b / 255.f, a / 255.f};
    }
    return Graphics::Color::black();
}

static Graphics::Color parseRGBFunction(const std::string& value)
{
    auto start = value.find('(');
    auto end = value.find(')');
    if (start == std::string::npos || end == std::string::npos)
        return Graphics::Color::black();

    auto inner = value.substr(start + 1, end - start - 1);
    auto numbers = parseNumberList(inner);
    if (numbers.size() >= 3)
    {
        return {numbers[0] / 255.f, numbers[1] / 255.f, numbers[2] / 255.f};
    }
    return Graphics::Color::black();
}

static const std::unordered_map<std::string, Graphics::Color>& namedColors()
{
    static const std::unordered_map<std::string, Graphics::Color> colors = {
        {"white", {1.f, 1.f, 1.f}},       {"black", {0.f, 0.f, 0.f}},
        {"red", {1.f, 0.f, 0.f}},         {"green", {0.f, 0.5f, 0.f}},
        {"blue", {0.f, 0.f, 1.f}},        {"yellow", {1.f, 1.f, 0.f}},
        {"orange", {1.f, 0.647f, 0.f}},   {"purple", {0.5f, 0.f, 0.5f}},
        {"gray", {0.5f, 0.5f, 0.5f}},     {"grey", {0.5f, 0.5f, 0.5f}},
        {"cyan", {0.f, 1.f, 1.f}},        {"magenta", {1.f, 0.f, 1.f}},
        {"lime", {0.f, 1.f, 0.f}},        {"maroon", {0.5f, 0.f, 0.f}},
        {"navy", {0.f, 0.f, 0.5f}},       {"olive", {0.5f, 0.5f, 0.f}},
        {"teal", {0.f, 0.5f, 0.5f}},      {"silver", {0.75f, 0.75f, 0.75f}},
        {"aqua", {0.f, 1.f, 1.f}},        {"fuchsia", {1.f, 0.f, 1.f}},
        {"coral", {1.f, 0.498f, 0.314f}}, {"salmon", {0.98f, 0.502f, 0.447f}},
        {"gold", {1.f, 0.843f, 0.f}},     {"pink", {1.f, 0.753f, 0.796f}},
    };
    return colors;
}

ColorResult parseColor(const std::string& value)
{
    if (value.empty() || Strings::toLower(value) == "none")
        return {{}, true};

    if (value[0] == '#')
        return {parseHexColor(value.substr(1)), false};

    if (value.substr(0, 4) == "rgb(")
        return {parseRGBFunction(value), false};

    auto lower = Strings::toLower(value);
    auto& colors = namedColors();
    auto it = colors.find(lower);
    if (it != colors.end())
        return {it->second, false};

    return {Graphics::Color::black(), false};
}

namespace
{
void parseTranslate(NumberReader& reader, Transform& result)
{
    result.translateX = reader.readFloat();
    result.translateY = reader.readFloat();
}

void parseScale(NumberReader& reader, Transform& result)
{
    result.scaleX = reader.readFloat();
    auto saved = reader.pos;
    result.scaleY = reader.readFloat();
    if (reader.pos == saved)
        result.scaleY = result.scaleX;
}

void parseRotate(NumberReader& reader, Transform& result)
{
    result.rotateDeg = reader.readFloat();
}

void skipWhitespace(const std::string& value, size_t& pos)
{
    while (pos < value.size()
           && std::isspace(static_cast<unsigned char>(value[pos])))
    {
        ++pos;
    }
}

bool advanceToArgumentList(const std::string& value, size_t& pos)
{
    auto open = value.find('(', pos);
    if (open == std::string::npos)
        return false;
    pos = open + 1;
    return true;
}

void advancePastClosingParen(const std::string& value, size_t& pos)
{
    pos = value.find(')', pos);
    if (pos != std::string::npos)
        ++pos;
}

using TransformFunctionParser = void (*)(NumberReader&, Transform&);

struct TransformFunction
{
    std::string_view name;
    TransformFunctionParser parse;
};

constexpr TransformFunction transformFunctions[] = {
    {"translate", &parseTranslate},
    {"scale", &parseScale},
    {"rotate", &parseRotate},
};

TransformFunctionParser matchTransformFunction(const std::string& value, size_t pos)
{
    for (const auto& fn: transformFunctions)
        if (value.compare(pos, fn.name.size(), fn.name) == 0)
            return fn.parse;
    return nullptr;
}
} // namespace

Transform parseTransform(const std::string& value)
{
    auto result = Transform();
    auto pos = size_t {0};

    while (pos < value.size())
    {
        skipWhitespace(value, pos);
        if (pos >= value.size())
            break;

        auto parse = matchTransformFunction(value, pos);
        if (parse == nullptr)
        {
            ++pos;
            continue;
        }

        if (!advanceToArgumentList(value, pos))
            break;

        auto reader = NumberReader {value, pos};
        parse(reader, result);
        pos = reader.pos;
        advancePastClosingParen(value, pos);
    }

    return result;
}

Vector<float> parseNumberList(const std::string& value)
{
    auto result = Vector<float>();
    auto reader = NumberReader {value, 0};

    while (reader.hasNumber())
    {
        auto start = reader.pos;
        auto num = reader.readFloat();
        if (reader.pos == start)
            break;
        result.add(num);
    }

    return result;
}

Vector<Graphics::Point> parsePointList(const std::string& value)
{
    auto numbers = parseNumberList(value);
    Vector<Graphics::Point> points;
    for (auto i = 0; i + 1 < numbers.size(); i += 2)
        points.add({numbers[i], numbers[i + 1]});
    return points;
}

} // namespace eacp::SVG
