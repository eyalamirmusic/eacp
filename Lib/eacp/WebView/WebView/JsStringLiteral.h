#pragma once

#include "../Common.h"

namespace eacp::Graphics
{

// Escapes a string for safe injection into evaluated script. Defined once
// here so unity builds can't ODR-clash on duplicate local copies.
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
