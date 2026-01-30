#include "SVGAttributes.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>

namespace eacp::SVG
{

static std::string toLower(std::string s)
{
    std::ranges::transform(
        s, s.begin(), [](auto c) { return (char)std::tolower((int)c); });
    return s;
}

static uint8_t hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<uint8_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return static_cast<uint8_t>(c - 'A' + 10);
    return 0;
}

static Graphics::Color parseHexColor(const std::string& hex)
{
    if (hex.size() == 3)
    {
        auto r = hexDigit(hex[0]);
        auto g = hexDigit(hex[1]);
        auto b = hexDigit(hex[2]);
        return {(r * 17) / 255.f, (g * 17) / 255.f, (b * 17) / 255.f};
    }
    if (hex.size() == 6)
    {
        auto r = hexDigit(hex[0]) * 16 + hexDigit(hex[1]);
        auto g = hexDigit(hex[2]) * 16 + hexDigit(hex[3]);
        auto b = hexDigit(hex[4]) * 16 + hexDigit(hex[5]);
        return {r / 255.f, g / 255.f, b / 255.f};
    }
    if (hex.size() == 8)
    {
        auto r = hexDigit(hex[0]) * 16 + hexDigit(hex[1]);
        auto g = hexDigit(hex[2]) * 16 + hexDigit(hex[3]);
        auto b = hexDigit(hex[4]) * 16 + hexDigit(hex[5]);
        auto a = hexDigit(hex[6]) * 16 + hexDigit(hex[7]);
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
    if (value.empty() || toLower(value) == "none")
        return {{}, true};

    if (value[0] == '#')
        return {parseHexColor(value.substr(1)), false};

    if (value.substr(0, 4) == "rgb(")
        return {parseRGBFunction(value), false};

    auto lower = toLower(value);
    auto& colors = namedColors();
    auto it = colors.find(lower);
    if (it != colors.end())
        return {it->second, false};

    return {Graphics::Color::black(), false};
}

static float parseFloat(const std::string& s, size_t& pos)
{
    while (pos < s.size()
           && (std::isspace(static_cast<unsigned char>(s[pos])) || s[pos] == ','))
    {
        ++pos;
    }

    auto start = pos;
    if (pos < s.size() && (s[pos] == '-' || s[pos] == '+'))
        ++pos;
    while (pos < s.size()
           && (std::isdigit(static_cast<unsigned char>(s[pos])) || s[pos] == '.'))
    {
        ++pos;
    }

    if (pos == start)
        return 0.f;

    return std::stof(s.substr(start, pos - start));
}

Transform parseTransform(const std::string& value)
{
    Transform result;
    size_t pos = 0;

    while (pos < value.size())
    {
        while (pos < value.size()
               && std::isspace(static_cast<unsigned char>(value[pos])))
        {
            ++pos;
        }

        if (value.substr(pos, 9) == "translate")
        {
            pos = value.find('(', pos);
            if (pos == std::string::npos)
                break;
            ++pos;
            result.translateX = parseFloat(value, pos);
            result.translateY = parseFloat(value, pos);
            pos = value.find(')', pos);
            if (pos != std::string::npos)
                ++pos;
        }
        else if (value.substr(pos, 5) == "scale")
        {
            pos = value.find('(', pos);
            if (pos == std::string::npos)
                break;
            ++pos;
            result.scaleX = parseFloat(value, pos);
            auto saved = pos;
            result.scaleY = parseFloat(value, pos);
            if (pos == saved)
                result.scaleY = result.scaleX;
            pos = value.find(')', pos);
            if (pos != std::string::npos)
                ++pos;
        }
        else if (value.substr(pos, 6) == "rotate")
        {
            pos = value.find('(', pos);
            if (pos == std::string::npos)
                break;
            ++pos;
            result.rotateDeg = parseFloat(value, pos);
            pos = value.find(')', pos);
            if (pos != std::string::npos)
                ++pos;
        }
        else
        {
            ++pos;
        }
    }
    return result;
}

std::vector<float> parseNumberList(const std::string& value)
{
    std::vector<float> result;
    size_t pos = 0;
    while (pos < value.size())
    {
        while (pos < value.size()
               && (std::isspace(static_cast<unsigned char>(value[pos]))
                   || value[pos] == ','))
        {
            ++pos;
        }
        if (pos >= value.size())
            break;

        auto start = pos;
        auto num = parseFloat(value, pos);
        if (pos > start)
            result.push_back(num);
        else
            break;
    }
    return result;
}

std::vector<Graphics::Point> parsePointList(const std::string& value)
{
    auto numbers = parseNumberList(value);
    std::vector<Graphics::Point> points;
    for (size_t i = 0; i + 1 < numbers.size(); i += 2)
        points.push_back({numbers[i], numbers[i + 1]});
    return points;
}

} // namespace eacp::SVG
