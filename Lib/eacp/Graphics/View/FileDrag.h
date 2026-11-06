#pragma once

/** @file Options and result for dragging files out of a view to the OS. */

#include "../Image/Image.h"

#include <functional>

namespace eacp::Graphics
{

enum class FileDragResult
{
    Cancelled,
    Copied,
    Moved
};

// Options for View::startFileDrag. `imageScale` is image pixels per point.
struct FileDragOptions
{
    bool canMove = false;
    Image image;
    float imageScale = 1.f;
    std::function<void(FileDragResult)> onFinished;
};

} // namespace eacp::Graphics
