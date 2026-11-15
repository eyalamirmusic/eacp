#include "App.h"

#include <emscripten/em_js.h>

namespace
{
// A new tab in a page; node has nowhere to open one.
// clang-format off
EM_JS_DEPS(eacpWebApp, "$UTF8ToString");

EM_JS(void, eacpWebOpenUrl, (const char* url), {
    if (typeof window !== 'undefined')
        window.open(UTF8ToString(url), '_blank', 'noopener');
});
// clang-format on
} // namespace

namespace eacp::Apps
{
void setDockIconVisible(bool) {}

void setAppBadge(const std::string&) {}

bool isSystemPoweringOff()
{
    return false;
}

void Detail::observeSystemPowerOff() {}

bool isDistributionSigned()
{
    return false;
}

void openExternalURL(const std::string& url)
{
    eacpWebOpenUrl(url.c_str());
}

std::optional<std::string> chooseFile(const FilePickerOptions&)
{
    return std::nullopt;
}

std::optional<std::string> chooseSaveFile(const FileSaveOptions&)
{
    return std::nullopt;
}

std::optional<std::string> chooseDirectory()
{
    return std::nullopt;
}
} // namespace eacp::Apps
