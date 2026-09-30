#include "App.h"

#include <emscripten/emscripten.h>

namespace eacp::Apps
{
namespace
{
// noopener, so the page opened cannot reach back into this one.
EM_JS(void, webOpenExternalURL, (const char* url), {
    window.open(UTF8ToString(url), '_blank', 'noopener');
});
} // namespace

// A tab has no Dock tile, badge, power-off announcement or signature.
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
    webOpenExternalURL(url.c_str());
}

// An <input type="file"> answers through a promise; these calls block.
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
