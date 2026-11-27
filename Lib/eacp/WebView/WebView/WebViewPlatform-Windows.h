#pragma once

#include "WebView.h"

#include <eacp/Core/Utils/WinInclude.h>

#include <wrl/client.h>
#include <WebView2.h>

namespace eacp::Graphics::detail
{
// The view's CoreWebView2, the Windows twin of wkWebViewOf. Empty until the
// view is in a window and WebView2 has finished creating it.
Microsoft::WRL::ComPtr<ICoreWebView2> coreWebView2Of(WebView& view);
} // namespace eacp::Graphics::detail
