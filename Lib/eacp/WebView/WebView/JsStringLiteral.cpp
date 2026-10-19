#include "JsStringLiteral.h"

#include <eacp/Core/Utils/Format.h>

namespace eacp::Graphics
{

std::string jsStringLiteral(std::string_view value)
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
                    out +=
                        Strings::format("\\u{:04x}", static_cast<unsigned char>(c));
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
