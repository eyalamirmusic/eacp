#pragma once

#include <functional>

struct android_app;

namespace eacp::Graphics::Android
{
// Null outside android_main.
android_app* getApp();

void setLifecycleHandler(std::function<void(bool resumed)> handler);
} // namespace eacp::Graphics::Android
