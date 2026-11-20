#include "KillContentProcess.h"

#import <WebKit/WKWebView.h>

#include <eacp/WebView/WebView/WebViewPlatform-Apple.h>

#include <csignal>

// Private, as WebKit's own tests use it: the pid of the view's WebContent process.
@interface WKWebView (EacpContentProcessTesting)
- (pid_t)_webProcessIdentifier;
@end

void killContentProcess(eacp::Graphics::WebView& webView)
{
    auto* wkWebView = eacp::Graphics::detail::wkWebViewOf(&webView);

    if (wkWebView != nil)
        kill([wkWebView _webProcessIdentifier], SIGKILL);
}
