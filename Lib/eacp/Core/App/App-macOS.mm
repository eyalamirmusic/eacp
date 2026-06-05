#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "App.h"

namespace eacp::Apps
{
void setDockIconVisible(bool visible)
{
    [NSApp setActivationPolicy:visible ? NSApplicationActivationPolicyRegular
                                       : NSApplicationActivationPolicyAccessory];
}

void openExternalURL(const std::string& url)
{
    auto* nsString = [NSString stringWithUTF8String:url.c_str()];

    if (nsString == nil)
        return;

    auto* nsUrl = [NSURL URLWithString:nsString];

    if (nsUrl == nil)
        return;

    [[NSWorkspace sharedWorkspace] openURL:nsUrl];
}

std::optional<std::string> chooseFile(const FilePickerOptions& options)
{
    auto* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.resolvesAliases = YES;

    if (! options.allowedExtensions.empty())
    {
        auto* types = [NSMutableArray<UTType*> array];
        for (const auto& extension : options.allowedExtensions)
        {
            auto* ext = [NSString stringWithUTF8String:extension.c_str()];
            if (ext == nil)
                continue;

            auto* type = [UTType typeWithFilenameExtension:ext];
            if (type != nil)
                [types addObject:type];
        }
        panel.allowedContentTypes = types;
    }

    if ([panel runModal] != NSModalResponseOK)
        return std::nullopt;

    auto* url = panel.URLs.firstObject;

    if (url == nil)
        return std::nullopt;

    return std::string(url.fileSystemRepresentation);
}

std::optional<std::string> chooseDirectory()
{
    auto* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.allowsMultipleSelection = NO;
    panel.resolvesAliases = YES;

    if ([panel runModal] != NSModalResponseOK)
        return std::nullopt;

    auto* url = panel.URLs.firstObject;

    if (url == nil)
        return std::nullopt;

    return std::string(url.fileSystemRepresentation);
}
} // namespace eacp::Apps
