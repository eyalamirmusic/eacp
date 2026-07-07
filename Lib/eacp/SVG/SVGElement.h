#pragma once

#include <eacp/Core/Utils/Strings.h>

#include <eacp/Core/Utils/Containers.h>
#include <string>
#include <unordered_map>

namespace eacp::SVG
{

struct SVGElement
{
    std::string attr(const std::string& name, const std::string& fallback = "") const
    {
        auto it = attributes.find(name);
        if (it != attributes.end())
            return it->second;
        return fallback;
    }

    float numAttr(const std::string& name, float fallback = 0.f) const
    {
        auto it = attributes.find(name);
        if (it == attributes.end())
            return fallback;
        return Strings::parseFloatOr(it->second, fallback);
    }

    std::string tag;
    std::unordered_map<std::string, std::string> attributes;
    Vector<SVGElement> children;
    std::string textContent;
};

} // namespace eacp::SVG
