#pragma once

#include "../Common.h"

namespace eacp::GPU
{
class Device;
class ShaderLibrary;

// A compiled pipeline state built from a library's kernel entry point; create
// via Device::makeComputePipeline.
class ComputePipeline
{
public:
    ComputePipeline(Device& device, const ShaderLibrary& library);

    bool isValid() const;

    void* nativeState() const;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
