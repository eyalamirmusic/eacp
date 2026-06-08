#pragma once

#include <eacp/Core/Utils/Common.h>

#include <string>

namespace eacp::GPU
{
class Device;
struct ShaderSource;

// A compiled shader library (MTLLibrary on Metal). Holds the entry-point names
// it was built with so a pipeline only needs to reference the library.
class ShaderLibrary
{
public:
    ShaderLibrary(Device& device, const ShaderSource& source);

    const std::string& vertexEntry() const { return vertexEntryName; }
    const std::string& fragmentEntry() const { return fragmentEntryName; }
    const std::string& computeEntry() const { return computeEntryName; }

    bool isValid() const;

    // Opaque native handle for cross-translation-unit use by the pipeline.
    void* nativeLibrary() const;

private:
    std::string vertexEntryName;
    std::string fragmentEntryName;
    std::string computeEntryName;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
