#pragma once

#import <AppKit/AppKit.h>

#include "FileDrag.h"

#include <string>

namespace eacp::Graphics
{

NSArray<NSDraggingItem*>* makeFileDraggingItems(NSView* view,
                                                NSPoint anchor,
                                                const Vector<std::string>& paths,
                                                const Image& image = {},
                                                float imageScale = 1.f);

FileDragResult fileDragResultFor(NSDragOperation operation);

} // namespace eacp::Graphics
