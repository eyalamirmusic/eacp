#pragma once

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

struct FileDragOptions
{
    bool canMove = false;
    Image image;
    float imageScale = 1.f;
    std::function<void(FileDragResult)> onFinished;
};

} // namespace eacp::Graphics
