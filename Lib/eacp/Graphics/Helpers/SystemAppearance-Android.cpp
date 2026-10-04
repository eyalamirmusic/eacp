#include "SystemAppearance.h"

#include "../Window/Android.h"

#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/native_activity.h>
#include <android_native_app_glue.h>

#include <memory>

namespace eacp::Graphics
{
namespace
{
struct ConfigurationDeleter
{
    void operator()(AConfiguration* configuration) const
    {
        AConfiguration_delete(configuration);
    }
};

using Configuration = std::unique_ptr<AConfiguration, ConfigurationDeleter>;
} // namespace

// The glue's AConfiguration is only safe on the app thread between commands.
bool isSystemDarkMode()
{
    auto* app = Android::getApp();

    if (app == nullptr || app->activity == nullptr
        || app->activity->assetManager == nullptr)
        return false;

    auto configuration = Configuration {AConfiguration_new()};

    if (configuration == nullptr)
        return false;

    AConfiguration_fromAssetManager(configuration.get(),
                                    app->activity->assetManager);

    return AConfiguration_getUiModeNight(configuration.get())
           == ACONFIGURATION_UI_MODE_NIGHT_YES;
}
} // namespace eacp::Graphics
