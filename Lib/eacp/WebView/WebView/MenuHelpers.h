#pragma once

#include <eacp/Graphics/Menu/Menu.h>

namespace eacp::Graphics
{
namespace WebHelpers
{
void zoomInFocusedWebView();
void zoomOutFocusedWebView();
void resetSizedFocusedWebView();

} // namespace WebHelpers

MenuBar buildDefaultWebViewMenuBar();
} // namespace eacp::Graphics
