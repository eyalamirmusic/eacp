#pragma once

#include <memory>
#include "GraphicsContext.h"


namespace eacp::Graphics
{

class View
{
public:
    View();

    virtual void paint(Context& ctx) = 0;

    void* getHandle();

private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
}