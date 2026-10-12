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

    const std::string& vertexEntry() const;
    const std::string& fragmentEntry() const;
    const std::string& computeEntry() const;

    // False for a vertex-only library (ShaderSource::withFragment with an empty
    // entry), whose pipelines have no fragment stage.
    bool hasFragment() const;

    // The group the kernel was emitted for, carried to the pipeline the pass
    // dispatches through.
    ThreadGroupShape threadGroupShape() const;

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
