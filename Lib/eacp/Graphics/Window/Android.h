#pragma once

/** @file Internal: Android only. The android_app handed to android_main. */

struct android_app;

namespace eacp::Graphics::Android
{
// Null outside android_main.
android_app* getApp();
} // namespace eacp::Graphics::Android
