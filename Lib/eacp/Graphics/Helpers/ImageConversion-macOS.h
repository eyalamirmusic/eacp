#pragma once

#import <Cocoa/Cocoa.h>

#include <eacp/Graphics/Image/Image.h>

namespace eacp::Graphics
{
// Wraps the Image's straight RGBA pixels in an autoreleased NSImage at the
// source's natural pixel size. Returns nil on an empty image.
NSImage* toNSImage(const Image& image);
} // namespace eacp::Graphics
