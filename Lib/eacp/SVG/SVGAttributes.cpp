#include "SVGAttributes.h"
#include "NumberReader.h"

#include <algorithm>
#include <sstream>

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

std::string_view readFunctionName(const std::string& value, size_t& pos)
{
    auto start = pos;

    while (pos < value.size()
           && std::isalpha(static_cast<unsigned char>(value[pos])))
        ++pos;

    return std::string_view {value}.substr(start, pos - start);
}

// Hands each function of a transform list, in the order written, to `consume` as
// its name and a reader positioned on its arguments.
template <typename Consumer>
void forEachTransformFunction(const std::string& value, Consumer&& consume)
{
    auto pos = size_t {0};

    while (pos < value.size())
    {
        skipWhitespace(value, pos);
        if (pos >= value.size())
            break;

        auto name = readFunctionName(value, pos);

        if (name.empty())
        {
            ++pos;
            continue;
        }

        if (!advanceToArgumentList(value, pos))
            break;

        auto reader = NumberReader {value, pos};
        consume(name, reader);
        pos = reader.pos;

        advancePastClosingParen(value, pos);
    }
}

// The second of a pair that may be written once for both, which is how scale and
// translate are allowed to be spelled.
float readOptionalFloat(NumberReader& reader, float fallback)
{
    return reader.hasNumber() ? reader.readFloat() : fallback;
}

float toRadians(float degrees)
{
    return degrees * GPUWidgets::pi / 180.f;
}
} // namespace

Transform parseTransform(const std::string& value)
{
    auto result = Transform();

    forEachTransformFunction(value,
                             [&result](std::string_view name, NumberReader& reader)
                             {
                                 if (name == "translate")
                                 {
                                     result.translateX = reader.readFloat();
                                     result.translateY =
                                         readOptionalFloat(reader, 0.f);
                                 }
                                 else if (name == "scale")
                                 {
                                     result.scaleX = reader.readFloat();
                                     result.scaleY =
                                         readOptionalFloat(reader, result.scaleX);
                                 }
                                 else if (name == "rotate")
                                 {
                                     result.rotateDeg = reader.readFloat();
                                 }
                             });

    return result;
}

namespace
{
GPUWidgets::AffineTransform readTransformFunction(std::string_view name,
                                                  NumberReader& reader)
{
    using Affine = GPUWidgets::AffineTransform;

    if (name == "translate")
    {
        auto x = reader.readFloat();
        return Affine::translation(x, readOptionalFloat(reader, 0.f));
    }

    if (name == "scale")
    {
        auto x = reader.readFloat();
        return Affine::scaling(x, readOptionalFloat(reader, x));
    }

    if (name == "rotate")
    {
        auto radians = toRadians(reader.readFloat());

        if (!reader.hasNumber())
            return Affine::rotation(radians);

        // The three-argument form rotates about a point rather than the origin,
        // which is how a document spins a shape in place without first working
        // out where the origin put it.
        auto centreX = reader.readFloat();
        auto centreY = readOptionalFloat(reader, 0.f);

        return Affine::rotationAbout(radians, {centreX, centreY});
    }

    if (name == "skewX")
        return Affine::skew(toRadians(reader.readFloat()), 0.f);

    if (name == "skewY")
        return Affine::skew(0.f, toRadians(reader.readFloat()));

    if (name == "matrix")
    {
        auto result = Affine {};
        result.a = reader.readFloat();
        result.b = reader.readFloat();
        result.c = reader.readFloat();
        result.d = reader.readFloat();
        result.tx = reader.readFloat();
        result.ty = reader.readFloat();

        return result;
    }

    return {};
}
} // namespace

GPUWidgets::AffineTransform parseTransformMatrix(const std::string& value)
{
    auto result = GPUWidgets::AffineTransform {};

    forEachTransformFunction(
        value,
        [&result](std::string_view name, NumberReader& reader)
        {
            // A list composes right to left: each function transforms the
            // coordinate system the next one is written in, so
            // translate(..) rotate(..) rotates first and then translates. Which
            // makes a newly read function the one applied *before* everything
            // read so far.
            result = readTransformFunction(name, reader).then(result);
        });

    return result;
}

namespace
{
PreserveAspectRatio::Align alignFromKeyword(std::string_view keyword)
{
    if (keyword == "Min")
        return PreserveAspectRatio::Align::Min;

    if (keyword == "Max")
        return PreserveAspectRatio::Align::Max;

    return PreserveAspectRatio::Align::Mid;
}

// "xMidYMax" and its eight siblings, which are the only alignment words the
// grammar has: an x keyword and a y keyword run together, eight characters,
// always in that order.
bool readAlignKeyword(const std::string& token, PreserveAspectRatio& result)
{
    if (token.size() != 8 || token[0] != 'x' || token[4] != 'Y')
        return false;

    result.uniform = true;
    result.x = alignFromKeyword(std::string_view {token}.substr(1, 3));
    result.y = alignFromKeyword(std::string_view {token}.substr(5, 3));

    return true;
}

template <typename Consumer>
void forEachToken(const std::string& value, Consumer&& consume)
{
    auto pos = size_t {0};

    while (pos < value.size())
    {
        skipWhitespace(value, pos);

        auto start = pos;

        while (pos < value.size()
               && !std::isspace(static_cast<unsigned char>(value[pos])))
            ++pos;

        if (pos > start)
            consume(value.substr(start, pos - start));
    }
}

std::string trimmed(const std::string& value)
{
    auto first = value.find_first_not_of(" \t\r\n");

    if (first == std::string::npos)
        return {};

    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
} // namespace

PreserveAspectRatio parsePreserveAspectRatio(const std::string& value)
{
    auto result = PreserveAspectRatio {};

    forEachToken(value,
                 [&result](const std::string& token)
                 {
                     // "defer" is a legacy word that only ever meant anything on
                     // an <image>, so it is read past rather than acted on.
                     if (token == "none")
                         result.uniform = false;
                     else if (token == "meet")
                         result.slice = false;
                     else if (token == "slice")
                         result.slice = true;
                     else if (token != "defer")
                         readAlignKeyword(token, result);
                 });

    return result;
}

GPUWidgets::AffineTransform viewBoxTransform(const Graphics::Rect& viewBox,
                                             const Graphics::Rect& viewport,
                                             const PreserveAspectRatio& fit)
{
    using Affine = GPUWidgets::AffineTransform;

    if (viewBox.w <= 0.f || viewBox.h <= 0.f)
        return {};

    auto scaleX = viewport.w / viewBox.w;
    auto scaleY = viewport.h / viewBox.h;

    if (fit.uniform)
    {
        // The smaller scale fits the box inside the viewport and the larger one
        // covers it, which is the whole of the difference between meet and
        // slice.
        auto both = fit.slice ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);

        scaleX = both;
        scaleY = both;
    }

    auto offsetFor = [](PreserveAspectRatio::Align align, float spare)
    {
        if (align == PreserveAspectRatio::Align::Min)
            return 0.f;

        return align == PreserveAspectRatio::Align::Mid ? spare * 0.5f : spare;
    };

    // Negative under slice, which is what puts the overflow on both sides.
    auto x = viewport.x + offsetFor(fit.x, viewport.w - viewBox.w * scaleX);
    auto y = viewport.y + offsetFor(fit.y, viewport.h - viewBox.h * scaleY);

    return Affine::translation(-viewBox.x, -viewBox.y)
        .then(Affine::scaling(scaleX, scaleY))
        .then(Affine::translation(x, y));
}

std::unordered_map<std::string, std::string>
    parseStyleDeclarations(const std::string& value)
{
    auto declarations = std::unordered_map<std::string, std::string> {};
    auto pos = size_t {0};

    while (pos < value.size())
    {
        auto end = value.find(';', pos);

        if (end == std::string::npos)
            end = value.size();

        auto colon = value.find(':', pos);

        if (colon != std::string::npos && colon < end)
        {
            auto property = trimmed(value.substr(pos, colon - pos));

            if (!property.empty())
                declarations[property] =
                    trimmed(value.substr(colon + 1, end - colon - 1));
        }

        pos = end + 1;
    }

    return declarations;
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

std::string parsePaintReference(const std::string& value)
{
    auto open = value.find("url(");

    if (open == std::string::npos)
        return {};

    auto close = value.find(')', open);

    if (close == std::string::npos)
        return {};

    auto reference = trimmed(value.substr(open + 4, close - open - 4));

    // Quoted is legal and common: url('#id') and url("#id") mean what url(#id)
    // does.
    if (reference.size() >= 2
        && (reference.front() == '\'' || reference.front() == '"')
        && reference.back() == reference.front())
        reference = reference.substr(1, reference.size() - 2);

    if (!reference.empty() && reference.front() == '#')
        reference.erase(reference.begin());

    return reference;
}

std::string hrefId(const SVGElement& element)
{
    auto href = element.attr("href");

    if (href.empty())
        href = element.attr("xlink:href");

    return href.size() > 1 && href.front() == '#' ? href.substr(1) : std::string {};
}

} // namespace eacp::SVG
