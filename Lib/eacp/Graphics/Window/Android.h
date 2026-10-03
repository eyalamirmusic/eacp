#pragma once

struct android_app;

namespace eacp::Graphics::Android
{
// Null outside android_main.
android_app* getApp();
} // namespace eacp::Graphics::Android
