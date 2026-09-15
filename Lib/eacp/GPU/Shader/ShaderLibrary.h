#pragma once

#include "../Common.h"

#include "ShaderSource.h"

namespace eacp::GPU
{
class Device;

// A compiled shader library (MTLLibrary on Metal). Holds the entry-point names
// it was built with so a pipeline only needs to reference the library.
class ShaderLibrary
{
public:
    ShaderLibrary(Device& device, const ShaderSource& source);

    const std::string& vertexEntry() const { return vertexEntryName; }
    const std::string& fragmentEntry() const { return fragmentEntryName; }
    const std::string& computeEntry() const { return computeEntryName; }

    // The group the kernel was emitted for, carried to the pipeline the pass
    // dispatches through.
    ThreadGroupShape threadGroupShape() const { return groupShape; }

    bool isValid() const;

    // Opaque native handle for cross-translation-unit use by the pipeline.
    void* nativeLibrary() const;

private:
    std::string vertexEntryName;
    std::string fragmentEntryName;
    std::string computeEntryName;
    ThreadGroupShape groupShape;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
