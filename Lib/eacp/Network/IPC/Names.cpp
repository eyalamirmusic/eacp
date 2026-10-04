#include "Names.h"

namespace eacp::IPC::detail
{

std::string foldToFileName(std::string_view name)
{
    auto result = std::string {};
    result.reserve(name.size());

    for (auto character: name)
    {
        auto isSafe = (character >= 'a' && character <= 'z')
                      || (character >= 'A' && character <= 'Z')
                      || (character >= '0' && character <= '9') || character == '.'
                      || character == '-' || character == '_';

        result += isSafe ? character : '_';
    }

    return result;
}

} // namespace eacp::IPC::detail
