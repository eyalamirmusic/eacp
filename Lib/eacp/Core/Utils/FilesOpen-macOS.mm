#import <AppKit/AppKit.h>

#include "FilesPlatform.h"

#include "../ObjC/Strings.h"

namespace eacp::Detail
{
// Launch Services opens a directory in Finder and a file (or an .app bundle)
// with its default application; openURL: answers once the request is handed
// off, without waiting for the application it starts.
bool openWithSystem(const FilePath& target)
{
    @autoreleasepool
    {
        auto* path = Strings::toNSString(target.str());

        if (path == nil)
            return false;

        return [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]];
    }
}

// An empty root path asks Finder for a new window on the item's own folder,
// with the item selected.
bool revealInFileManager(const FilePath& target)
{
    @autoreleasepool
    {
        auto* path = Strings::toNSString(target.str());

        if (path == nil)
            return false;

        return [[NSWorkspace sharedWorkspace] selectFile:path
                                inFileViewerRootedAtPath:@""];
    }
}
} // namespace eacp::Detail
