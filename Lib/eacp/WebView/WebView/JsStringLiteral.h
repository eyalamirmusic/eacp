#pragma once

#include <cstdio>
#include <string>
#include <string_view>

namespace eacp::Graphics
{

// Escapes a string as a double-quoted JS/JSON literal so it can be
// injected safely into evaluated script. Shared by the bridge wiring
// (Bridge.cpp) and the platform WebView glue (WebView-Shared.cpp);
// kept in one inline definition so unity builds don't merge two
// identical anonymous-namespace copies into an ODR clash.
inline std::string jsStringLiteral(std::string_view value)
{
    auto out = std::string {"\""};
    out.reserve(value.size() + 2);

    for (auto c: value)
    {
        switch (c)
        {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buf[8];
                    std::snprintf(
                        buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                }
                else
                {
                    out += c;
                }
                break;
        }
    }
    out += '"';
    return out;
}

} // namespace eacp::Graphics
