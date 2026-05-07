#pragma once

#include <eacp/Graphics/Primitives/Primitives.h>
#include <ea_data_structures/Structures/Vector.h>
#include <string>

namespace eacp::SVG
{

struct ColorResult
{
    Graphics::Color color;
    bool isNone = false;
};

ColorResult parseColor(const std::string& value);

struct Transform
{
    float translateX = 0.f;
    float translateY = 0.f;
    float scaleX = 1.f;
    float scaleY = 1.f;
    float rotateDeg = 0.f;
};

Transform parseTransform(const std::string& value);

EA::Vector<float> parseNumberList(const std::string& value);

EA::Vector<Graphics::Point> parsePointList(const std::string& value);

} // namespace eacp::SVG
