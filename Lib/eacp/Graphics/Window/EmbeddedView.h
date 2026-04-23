#pragma once

#include "../View/View.h"
#include <eacp/Core/Utils/Common.h>
#include <functional>

namespace eacp::Graphics
{

struct EmbeddedViewOptions
{
    int width = 640;
    int height = 400;
};

class EmbeddedView
{
public:
    EmbeddedView(void* hostParentHandle,
                 const EmbeddedViewOptions& optionsToUse = {});
    ~EmbeddedView();

    void setContentView(View& view);
    void setSize(int width, int height);
    void* getHandle();

    std::function<void(int width, int height)> onResizeRequest;

private:
    EmbeddedViewOptions options;

    struct Native;
    Pimpl<Native> impl;
};

} // namespace eacp::Graphics
