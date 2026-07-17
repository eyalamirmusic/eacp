#pragma once

#include <string>
#include <string_view>

// Shared by the lock and the channel so one name always folds to one file,
// whichever primitive spelled it.
namespace eacp::IPC::detail
{

// Folds anything a filename cannot carry, so a name can never reach outside
// the directory it is planted in. Separators go too, which is what makes
// traversal impossible rather than merely unlikely.
inline std::string foldToFileName(std::string_view name)
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
