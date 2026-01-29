#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace eacp::SVG
{

struct SVGElement
{
    std::string tag;
    std::unordered_map<std::string, std::string> attributes;
    std::vector<SVGElement> children;
    std::string textContent;

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
        if (it != attributes.end())
        {
            try
            {
                return std::stof(it->second);
            }
            catch (...)
            {
                return fallback;
            }
        }
        return fallback;
    }
};

} // namespace eacp::SVG
