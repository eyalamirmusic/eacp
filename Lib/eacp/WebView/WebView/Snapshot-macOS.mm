#import <AppKit/AppKit.h>
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
Bytes encodeAsPng(NSImage* image, std::string& error)
{
    // NSImage may wrap a vector representation (PDF, EPS); round-tripping
    // through TIFF guarantees a bitmap rep that representationUsingType:
    // accepts.
    auto* tiff = [image TIFFRepresentation];
    if (tiff == nil)
    {
        error = "NSImage has no TIFF representation";
        return {};
    }

    auto* rep = [NSBitmapImageRep imageRepWithData:tiff];
    if (rep == nil)
    {
        error = "could not build NSBitmapImageRep from snapshot";
        return {};
    }

    auto* png = [rep representationUsingType:NSBitmapImageFileTypePNG
                                  properties:@{}];
    if (png == nil)
    {
        error = "NSBitmapImageRep PNG encoding failed";
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
                         completionHandler:^(NSImage* image, NSError* error) {
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
