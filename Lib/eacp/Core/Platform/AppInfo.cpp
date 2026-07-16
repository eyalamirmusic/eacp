#include "Platform.h"

#include <Miro/Reflect.h>
#include <ResEmbed/ResEmbed.h>

#include <exception>
#include <string>

namespace eacp::Platform
{
namespace
{
struct AppInfo
{
    std::string name;
    std::string version;

    MIRO_REFLECT(name, version)
};

AppInfo loadAppInfo()
{
    if (auto resource = ResEmbed::get("AppInfo.json", "AppInfo"))
        return Miro::createFromJSONString<AppInfo>(resource.toStringView());

    return {};
}

const AppInfo& appInfo()
{
    static const AppInfo info = loadAppInfo();
    return info;
}
} // namespace

std::string_view getAppName()
{
    return appInfo().name;
}

std::string_view getAppVersion()
{
    return appInfo().version;
}
} // namespace eacp::Platform
