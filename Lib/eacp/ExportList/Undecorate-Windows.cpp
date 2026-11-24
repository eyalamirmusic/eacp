#include "ExportList.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <dbghelp.h>

#include <array>

namespace eacp::ExportList
{
namespace
{
constexpr auto standardScopeTerminator = std::string_view {"std@@"};
constexpr auto standardPrefix = std::string_view {"std::"};
constexpr auto bufferSize = std::size_t {4096};

using NameBuffer = std::array<char, bufferSize>;

// A name whose outermost scope is std spells it out, followed by the "@@" that
// ends the scope list: a back-reference could only repeat a scope already
// spelled at that level, and template arguments keep a table of their own. So
// a name without "std@@" never needs the (slow) undecorator.
bool mayBeInStandardNamespace(std::string_view decoratedName)
{
    return decoratedName.starts_with('?')
           && decoratedName.find(standardScopeTerminator) != std::string_view::npos;
}

// dbghelp wants a NUL-terminated name; the names are views into larger blocks.
std::string_view undecoratedQualifiedName(std::string_view decoratedName,
                                          NameBuffer& input,
                                          NameBuffer& output)
{
    if (decoratedName.size() >= input.size())
        return {};

    decoratedName.copy(input.data(), decoratedName.size());
    input[decoratedName.size()] = '\0';

    auto length = UnDecorateSymbolName(input.data(),
                                       output.data(),
                                       static_cast<DWORD>(output.size()),
                                       UNDNAME_NAME_ONLY);
    return {output.data(), length};
}
} // namespace

bool isInStandardNamespace(std::string_view decoratedName)
{
    if (!mayBeInStandardNamespace(decoratedName))
        return false;

    thread_local auto input = NameBuffer {};
    thread_local auto output = NameBuffer {};
    return undecoratedQualifiedName(decoratedName, input, output)
        .starts_with(standardPrefix);
}

std::string undecorate(std::string_view decoratedName)
{
    thread_local auto input = NameBuffer {};
    thread_local auto output = NameBuffer {};
    auto name = undecoratedQualifiedName(decoratedName, input, output);
    return std::string {name.empty() ? decoratedName : name};
}
} // namespace eacp::ExportList
