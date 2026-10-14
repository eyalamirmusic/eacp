#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include "Common.h"

#include <eacp/Graphics/View/FileDragOut-macOS.h>

using namespace nano;
using namespace eacp::Graphics;

namespace FileDragTests
{
namespace
{
NSView* nativeViewOf(View& view)
{
    return (__bridge NSView*) view.getHandle();
}

id<NSDraggingSource> sourceOf(View& view)
{
    return (id<NSDraggingSource>) nativeViewOf(view);
}

NSDraggingSession* noSession()
{
    return (NSDraggingSession*) nil;
}
} // namespace

auto tNativeViewIsADraggingSource = test("FileDrag/theNativeViewIsADraggingSource") = []
{
    auto view = View {};
    auto* native = nativeViewOf(view);

    check([native conformsToProtocol:@protocol(NSDraggingSource)]);
    check([native respondsToSelector:@selector(draggingSession:
                                         sourceOperationMaskForDraggingContext:)]);
    check([native respondsToSelector:@selector(draggingSession:
                                                  endedAtPoint:operation:)]);
};

auto tIdleSourceOffersACopy = test("FileDrag/anIdleSourceOffersOnlyACopy") = []
{
    auto view = View {};

    check([sourceOf(view) draggingSession:noSession()
              sourceOperationMaskForDraggingContext:NSDraggingContextOutsideApplication]
          == NSDragOperationCopy);
};

auto tOperationToResult = test("FileDrag/theEndOperationMapsToTheResult") = []
{
    check(fileDragResultFor(NSDragOperationNone) == FileDragResult::Cancelled);
    check(fileDragResultFor(NSDragOperationCopy) == FileDragResult::Copied);
    check(fileDragResultFor(NSDragOperationGeneric) == FileDragResult::Copied);
    check(fileDragResultFor(NSDragOperationMove) == FileDragResult::Moved);
    check(fileDragResultFor(NSDragOperationCopy | NSDragOperationMove)
          == FileDragResult::Moved);
};

auto tItemsCarryTheFiles = test("FileDrag/itemsAreFileURLsFannedOutFromTheAnchor") = []
{
    auto view = View {};
    view.setBounds({0.f, 0.f, 200.f, 200.f});

    auto items = makeFileDraggingItems(
        nativeViewOf(view), NSMakePoint(100, 100), {"/tmp/a.mid", "/tmp/b.mid"});

    check(items.count == 2);

    auto first = items[0].draggingFrame;
    auto second = items[1].draggingFrame;

    check(first.size.width == 64.0);
    check(first.size.height == 64.0);
    check(NSMidX(first) == 100.0);
    check(NSMidY(first) == 100.0);
    check(second.origin.x == first.origin.x + 8.0);
    check(second.origin.y == first.origin.y - 8.0);

    auto* url = (NSURL*) items[0].item;
    check([url isKindOfClass:[NSURL class]]);
    check([url.path isEqualToString:@"/tmp/a.mid"]);
};

auto tCustomImage = test("FileDrag/aDragImageIsSizedInPoints") = []
{
    auto view = View {};
    view.setBounds({0.f, 0.f, 200.f, 200.f});

    auto items = makeFileDraggingItems(
        nativeViewOf(view), NSMakePoint(50, 60), {"/tmp/a.mid"}, Image {40, 20}, 2.f);

    check(items.count == 1);

    auto frame = items[0].draggingFrame;

    check(frame.size.width == 20.0);
    check(frame.size.height == 10.0);
    check(NSMidX(frame) == 50.0);
    check(NSMidY(frame) == 60.0);
};
} // namespace FileDragTests
