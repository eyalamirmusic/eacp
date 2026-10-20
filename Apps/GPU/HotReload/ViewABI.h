#pragma once

#include <eacp/Core/Plugins/PluginExport.h>
#include <eacp/Graphics/Graphics.h>

// The module binds to the app's own eacp, so C++ types cross freely, but its
// image is unmapped a loop turn after the next reload. makeDemoView creates the
// one object the module is allowed to: a view whose code and vtable live in the
// image, handed to the host, which owns it and destroys it on the next reload
// before that image goes. The view keeps no static state and registers nothing
// with the app that could outlive it. Everything else in Views.cpp stays in its
// anonymous namespace, or it binds to the app's copy.

using ViewPointer = eacp::OwningPointer<eacp::Graphics::View>;
using ViewFactory = ViewPointer (*)();

EACP_HOT_RELOAD_EXPORT ViewPointer makeDemoView();

inline constexpr auto viewFactoryName = "makeDemoView";
