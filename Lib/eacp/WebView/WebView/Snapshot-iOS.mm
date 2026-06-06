#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>

#include "Snapshot-Apple.h"

#include <eacp/Core/Threads/EventLoop.h>

#include <cstdint>
#include <string>
#include <utility>

namespace eacp::Graphics::detail
{
namespace
{
Bytes encodeAsPng(UIImage* image, std::string& error)
{
    auto* png = UIImagePNGRepresentation(image);
    if (png == nil || png.length == 0)
    {
        error = "failed to encode UIImage as PNG";
        return {};
    }

    auto* bytes = static_cast<const std::uint8_t*>(png.bytes);
    auto result = Bytes();
    result.assign(bytes, bytes + png.length);
    return result;
}
} // namespace

void takeAppleSnapshot(WKWebView* webView, WebView::SnapshotCallback callback)
{
    auto* config = [[WKSnapshotConfiguration alloc] init];

    [webView takeSnapshotWithConfiguration:config
                         completionHandler:^(UIImage* image, NSError* error) {
                           Bytes bytes;
                           std::string errorStr;

                           if (error != nil)
                           {
                               auto* desc = error.localizedDescription.UTF8String;
                               errorStr = desc != nullptr ? desc : "snapshot failed";
                           }
                           else if (image != nil)
                               bytes = encodeAsPng(image, errorStr);
                           else
                               errorStr = "snapshot returned no image";

                           eacp::Threads::callAsync(
                               [callback, bytes = std::move(bytes), errorStr]() mutable
                               { callback(std::move(bytes), errorStr); });
                         }];
}
} // namespace eacp::Graphics::detail
