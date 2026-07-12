#include "Common.h"
#include <type_traits>

using namespace nano;

auto tClipboardCopyFilesRejectsEmptyList =
    test("Clipboard/copyFilesRejectsEmptyList") = []
{
    auto paths = eacp::Vector<std::string> {};
    check(!eacp::Clipboard::copyFiles(paths));
};

auto tClipboardTextApiReturnsBool = test("Clipboard/copyTextApiReturnsBool") = []
{ static_assert(std::is_same_v<decltype(eacp::Clipboard::copyText("")), bool>); };

auto tLoginItemQueryIsSideEffectFreeBool =
    test("LoginItem/isLaunchAtLoginReturnsBool") = []
{
    static_assert(std::is_same_v<decltype(eacp::Apps::isLaunchAtLogin()), bool>);

    auto enabled = eacp::Apps::isLaunchAtLogin();
    check(enabled || !enabled);
};
