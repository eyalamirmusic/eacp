#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "App.h"
#include "../ObjC/CFRef.h"

#include <cstring>
#include <mach-o/dyld.h>

namespace eacp::Apps
{
// Apple-issued (App Store / system) or the Developer ID leaf marker OID. Not
// plain "anchor apple generic": Xcode development certificates chain to it too.
static const auto distributionRequirement =
    CFSTR("anchor apple or (anchor apple generic and "
          "certificate leaf[field.1.2.840.113635.100.6.1.13] exists)");

static std::string currentExecutablePath()
{
    auto size = uint32_t {0};
    _NSGetExecutablePath(nullptr, &size);

    auto path = std::string(size, '\0');

    if (_NSGetExecutablePath(path.data(), &size) != 0)
        return {};

    path.resize(std::strlen(path.c_str()));
    return path;
}

bool isDistributionSigned()
{
    auto path = currentExecutablePath();

    if (path.empty())
        return false;

    auto url = CFRef(CFURLCreateFromFileSystemRepresentation(
        nullptr, (const UInt8*) path.c_str(), (CFIndex) path.size(), false));

    if (url.get() == nullptr)
        return false;

    SecStaticCodeRef codeRef = nullptr;

    if (SecStaticCodeCreateWithPath(url.get(), kSecCSDefaultFlags, &codeRef)
        != errSecSuccess)
        return false;

    auto code = CFRef(codeRef);
    SecRequirementRef requirementRef = nullptr;

    if (SecRequirementCreateWithString(distributionRequirement,
                                       kSecCSDefaultFlags,
                                       &requirementRef)
        != errSecSuccess)
        return false;

    auto requirement = CFRef(requirementRef);

    // Deliberately enforces neither expiry nor online revocation (see App.h).
    return SecStaticCodeCheckValidity(code.get(),
                                      kSecCSDoNotValidateResources,
                                      requirement.get())
           == errSecSuccess;
}

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

// An extension the system knows no type for is skipped, rather than dropping
// the whole filter.
static NSArray<UTType*>* contentTypesFor(const Vector<std::string>& extensions)
{
    auto* types = [NSMutableArray<UTType*> array];

    for (const auto& extension : extensions)
    {
        auto* ext = [NSString stringWithUTF8String:extension.c_str()];
        if (ext == nil)
            continue;

        auto* type = [UTType typeWithFilenameExtension:ext];
        if (type != nil)
            [types addObject:type];
    }

    return types;
}

std::optional<std::string> chooseFile(const FilePickerOptions& options)
{
    auto* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.resolvesAliases = YES;

    if (! options.allowedExtensions.empty())
        panel.allowedContentTypes = contentTypesFor(options.allowedExtensions);

    if ([panel runModal] != NSModalResponseOK)
        return std::nullopt;

    auto* url = panel.URLs.firstObject;

    if (url == nil)
        return std::nullopt;

    return std::string(url.fileSystemRepresentation);
}

std::optional<std::string> chooseSaveFile(const FileSaveOptions& options)
{
    auto* panel = [NSSavePanel savePanel];
    panel.canCreateDirectories = YES;
    panel.extensionHidden = NO;

    if (! options.allowedExtensions.empty())
        panel.allowedContentTypes = contentTypesFor(options.allowedExtensions);

    if (! options.suggestedName.empty())
    {
        auto* name = [NSString stringWithUTF8String:options.suggestedName.c_str()];
        if (name != nil)
            panel.nameFieldStringValue = name;
    }

    if ([panel runModal] != NSModalResponseOK)
        return std::nullopt;

    auto* url = panel.URL;

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
