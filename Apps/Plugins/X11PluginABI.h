#pragma once

// The C ABI between X11Host and X11Plugin, mirroring CLAP's gui and posix-fd
// extensions: the host hands over an X11 window id and a scale, watches one
// descriptor, and pumps the plugin whenever it is readable.
//
// Host and plugin each link their own copy of eacp, so nothing with C++
// layout crosses these four functions - ids are unsigned long, exactly as
// clap_window_t::x11 and VST3's "X11EmbedWindowID" hand them out.

namespace eacp::X11PluginABI
{
// Builds the plugin's UI as a child of `parentWindowId` at `scale` physical
// pixels per point. Non-zero on success; calling it twice without a close in
// between fails.
using Open = int (*)(unsigned long parentWindowId, double scale);

// The one descriptor the host watches for this plugin, or -1 while it is not
// open. Stable for the plugin's life.
using LoopFd = int (*)();

// Runs whatever the plugin has to do and returns. Called when that descriptor
// is readable, and harmless when nothing is - a host with only a timer or an
// idle callback calls it at its own rate instead.
using Pump = void (*)();

// Destroys the UI. The host takes the descriptor out of its loop first, and
// may unload the library once this returns.
using Close = void (*)();

inline constexpr auto openSymbol = "eacp_x11_plugin_open";
inline constexpr auto loopFdSymbol = "eacp_x11_plugin_loop_fd";
inline constexpr auto pumpSymbol = "eacp_x11_plugin_pump";
inline constexpr auto closeSymbol = "eacp_x11_plugin_close";
} // namespace eacp::X11PluginABI
