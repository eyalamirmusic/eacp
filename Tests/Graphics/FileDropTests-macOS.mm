#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include "Common.h"

// The AppKit half of OS file drops: a view that accepts them registers for file
// URLs, and the dragging-destination methods hand the paths and the position to
// the View. The drag itself needs a person; a stand-in NSDraggingInfo carrying
// a real pasteboard is the closest a test gets.

using namespace nano;
using namespace eacp::Graphics;

@interface EacpTestDraggingInfo : NSObject
@property(nonatomic, strong) NSPasteboard* pasteboard;
@property(nonatomic) NSPoint location;
@end

@implementation EacpTestDraggingInfo
- (void)dealloc
{
    [_pasteboard release];
    [super dealloc];
}

- (NSPasteboard*)draggingPasteboard
{
    return self.pasteboard;
}

- (NSPoint)draggingLocation
{
    return self.location;
}
@end

namespace
{
struct DropView final : View
{
    bool fileDragEnter(const eacp::Vector<std::string>& files, eacp::Graphics::Point position) override
    {
        ++enters;
        lastFiles = files;
        lastPosition = position;
        return wants;
    }

    bool fileDragMove(const eacp::Vector<std::string>&, eacp::Graphics::Point position) override
    {
        ++moves;
        lastPosition = position;
        return wants;
    }

    void fileDragExit() override { ++exits; }

    void filesDropped(const eacp::Vector<std::string>& files, eacp::Graphics::Point position) override
    {
        ++drops;
        lastFiles = files;
        lastPosition = position;
    }

    bool wants = true;
    int enters = 0;
    int moves = 0;
    int exits = 0;
    int drops = 0;
    eacp::Vector<std::string> lastFiles;
    eacp::Graphics::Point lastPosition;
};

NSView* nativeViewOf(View& view)
{
    return (__bridge NSView*) view.getHandle();
}

EacpTestDraggingInfo* dragOf(NSArray<NSString*>* paths, NSPoint location)
{
    auto* pasteboard = [NSPasteboard pasteboardWithUniqueName];
    auto* urls = [NSMutableArray array];

    for (NSString* path in paths)
        [urls addObject:[NSURL fileURLWithPath:path]];

    [pasteboard clearContents];
    [pasteboard writeObjects:urls];

    auto* info = [[EacpTestDraggingInfo alloc] init];
    info.pasteboard = pasteboard;
    info.location = location;

    return [info autorelease];
}

id<NSDraggingInfo> asDraggingInfo(EacpTestDraggingInfo* info)
{
    return (id<NSDraggingInfo>) info;
}
} // namespace

auto tRegistersForFileURLs = test("FileDrop/acceptingRegistersForFileURLs") = []
{
    auto view = View {};
    auto* native = nativeViewOf(view);

    check(![native.registeredDraggedTypes containsObject:NSPasteboardTypeFileURL]);

    view.setAcceptsFileDrops();
    check(view.getProperties().acceptsFileDrops);
    check([native.registeredDraggedTypes containsObject:NSPasteboardTypeFileURL]);

    view.setAcceptsFileDrops(false);
    check(![native.registeredDraggedTypes containsObject:NSPasteboardTypeFileURL]);
};

auto tDraggingReachesTheView =
    test("FileDrop/theDraggingDestinationMethodsReachTheView") = []
{
    auto view = DropView {};
    view.setBounds({0.f, 0.f, 200.f, 100.f});
    view.setAcceptsFileDrops();

    auto* native = (NSView<NSDraggingDestination>*) nativeViewOf(view);
    auto* info = dragOf(@[ @"/tmp/kick one.wav", @"/tmp/snare.wav" ], {30.f, 40.f});

    check([native draggingEntered:asDraggingInfo(info)] == NSDragOperationCopy);
    check(view.enters == 1);
    check(view.lastFiles.size() == 2);
    check(view.lastFiles[0] == "/tmp/kick one.wav");
    check(view.lastPosition.x == 30.f);

    view.wants = false;
    check([native draggingUpdated:asDraggingInfo(info)] == NSDragOperationNone);
    check(view.moves == 1);

    [native draggingExited:asDraggingInfo(info)];
    check(view.exits == 1);

    check([native performDragOperation:asDraggingInfo(info)]);
    check(view.drops == 1);
    check(view.lastFiles[1] == "/tmp/snare.wav");
};
