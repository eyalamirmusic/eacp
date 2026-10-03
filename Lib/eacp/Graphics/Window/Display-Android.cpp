#include "Android.h"
#include "Display.h"

#include <android/configuration.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

namespace eacp::Graphics
{
namespace
{
Display androidFallbackDisplay()
{
    const auto frame = Rect {0.f, 0.f, 400.f, 800.f};

    return {frame, frame, 1.f};
}
} // namespace

Display primaryDisplay()
{
    auto* app = Android::getApp();

    if (app == nullptr || app->window == nullptr)
        return androidFallbackDisplay();

    auto density = app->config != nullptr ? AConfiguration_getDensity(app->config)
                                          : ACONFIGURATION_DENSITY_MEDIUM;
    auto scale = density > 0 && density < ACONFIGURATION_DENSITY_ANY
                     ? (float) density / (float) ACONFIGURATION_DENSITY_MEDIUM
                     : 1.f;

    const auto frame = Rect {0.f,
                             0.f,
                             (float) ANativeWindow_getWidth(app->window) / scale,
                             (float) ANativeWindow_getHeight(app->window) / scale};

    return {frame, frame, scale};
}
} // namespace eacp::Graphics
