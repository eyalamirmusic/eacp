#pragma once

/** @file Registering the app to launch when the user logs in. */

namespace eacp::Apps
{

// Adds or removes this app from the user's login items.
void setLaunchAtLogin(bool enabled);
bool isLaunchAtLogin();

} // namespace eacp::Apps
