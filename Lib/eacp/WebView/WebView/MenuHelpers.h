#pragma once

/** @file A default menu bar with zoom items for apps built on a WebView. */

#include <eacp/Graphics/Menu/Menu.h>

namespace eacp::Graphics
{
namespace WebHelpers
{
/// Zoom actions on WebView::focused(); each does nothing when no WebView has focus.
void zoomInFocusedWebView();
void zoomOutFocusedWebView();
void resetSizedFocusedWebView();

} // namespace WebHelpers

/// A menu bar with a View menu of Zoom In, Zoom Out and Actual Size on the focused WebView.
MenuBar buildDefaultWebViewMenuBar();
} // namespace eacp::Graphics
