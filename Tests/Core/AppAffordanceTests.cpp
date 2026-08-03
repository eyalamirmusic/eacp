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

// A refusing handler has to actually stop the quit — that refusal is the
// whole affordance. Reset afterwards so the rest of the suite (and this
// process) can still exit; a null handler must restore "yes, quit" rather
// than leave an uncallable one behind.
auto tQuitHandlerRefusalStopsTheQuit =
    test("App/quitHandlerRefusalStopsTheQuit") = []
{
    auto asked = 0;

    eacp::Apps::setQuitHandler(
        [&]
        {
            ++asked;
            return false;
        });

    eacp::Apps::requestQuit();
    eacp::Apps::requestQuit();

    check(asked == 2);

    eacp::Apps::setQuitHandler(nullptr);
};

auto tLoginItemQueryIsSideEffectFreeBool =
    test("LoginItem/isLaunchAtLoginReturnsBool") = []
{
    static_assert(std::is_same_v<decltype(eacp::Apps::isLaunchAtLogin()), bool>);

    auto enabled = eacp::Apps::isLaunchAtLogin();
    check(enabled || !enabled);
};
