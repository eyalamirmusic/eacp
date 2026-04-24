#pragma once

#include "WebView.h"

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