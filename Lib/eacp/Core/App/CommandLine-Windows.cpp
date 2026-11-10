#include "../Utils/WinInclude.h"

#include "CommandLine.h"

#include <shellapi.h>

#include <memory>

namespace eacp::Apps::Detail
{
namespace
{
struct LocalFreeDeleter
{
    void operator()(wchar_t** list) const { LocalFree(list); }
};

Vector<std::wstring> processArguments()
{
    auto count = 0;
    auto list = std::unique_ptr<wchar_t*, LocalFreeDeleter> {
        CommandLineToArgvW(GetCommandLineW(), &count)};
    auto arguments = Vector<std::wstring> {};

    if (list == nullptr)
        return arguments;

    for (auto index = 0; index < count; ++index)
        arguments.emplace_back(list.get()[index]);

    return arguments;
}

// What the C runtime made of an argument for main(): the text in the ANSI code
// page, best fit, with '?' for what the code page cannot spell. This is the one
// conversion that is not UTF-8, so it does not go through Strings::narrow.
std::string inAnsiCodePage(std::wstring_view wide)
{
    if (wide.empty())
        return {};

    const auto length = static_cast<int>(wide.size());
    const auto size = WideCharToMultiByte(
        CP_ACP, 0, wide.data(), length, nullptr, 0, nullptr, nullptr);
    auto text = std::string(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_ACP, 0, wide.data(), length, text.data(), size, nullptr, nullptr);
    return text;
}
} // namespace

void recoverUtf8Arguments(Vector<std::string>& arguments)
{
    const auto process = processArguments();

    if (process.size() != arguments.size())
        return;

    for (auto index = 0; index < arguments.size(); ++index)
    {
        if (arguments[index] == inAnsiCodePage(process[index]))
            arguments[index] = Strings::narrow(process[index]);
    }
}
} // namespace eacp::Apps::Detail
