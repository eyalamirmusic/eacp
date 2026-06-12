#include "ElementIds.h"

#include <eacp/Core/Utils/Singleton.h>

namespace eacp::Graphics::ElementIds
{

namespace
{

struct State
{
    std::string name = "data-testid";
};

State& state()
{
    return Singleton::get<State>();
}

bool isIdChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
           || c == '-' || c == '_';
}

std::string escapeAttributeValue(std::string_view id)
{
    auto out = std::string {};
    out.reserve(id.size());

    for (auto c: id)
    {
        if (c == '"' || c == '\\')
            out.push_back('\\');
        out.push_back(c);
    }

    return out;
}

bool startsCompound(char previous)
{
    return previous == ' ' || previous == '\t' || previous == '\n'
           || previous == '\r' || previous == ',' || previous == '(';
}

} // namespace

std::string attributeName()
{
    return state().name;
}

void setAttributeName(std::string name)
{
    state().name = std::move(name);
}

std::string selectorFor(std::string_view id)
{
    return "[" + attributeName() + "=\"" + escapeAttributeValue(id) + "\"]";
}

std::string resolveSelector(std::string_view selector)
{
    auto out = std::string {};
    out.reserve(selector.size());

    auto atCompoundStart = true;
    auto bracketDepth = 0;

    for (auto i = std::size_t {0}; i < selector.size();)
    {
        auto c = selector[i];

        if (c == '[')
            ++bracketDepth;
        else if (c == ']' && bracketDepth > 0)
            --bracketDepth;

        if (c == '@' && atCompoundStart && bracketDepth == 0)
        {
            auto start = ++i;
            while (i < selector.size() && isIdChar(selector[i]))
                ++i;

            out += selectorFor(selector.substr(start, i - start));
            atCompoundStart = false;
            continue;
        }

        out.push_back(c);
        atCompoundStart = startsCompound(c);
        ++i;
    }

    return out;
}

} // namespace eacp::Graphics::ElementIds
