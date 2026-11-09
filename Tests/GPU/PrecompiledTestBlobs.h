#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <eacp/GPU/Shader/ShaderSource.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// The build played in-process for PrecompiledShaderTests: a source compiled to
// the platform's binary on the test machine, under the names a build would
// embed it by. `xcrun metal` on Apple (PrecompiledTestBlobs-Apple.cpp), FXC's
// DXBC on Windows (-Windows.cpp), eacp-spirv's SPIR-V on Linux and Android
// (-Linux.cpp).
namespace eacp::GPU::PrecompiledTestBlobs
{
struct Resource
{
    std::string name;
    Vector<std::uint8_t> bytes;
};

using Resources = Vector<Resource>;

inline Vector<std::uint8_t> bytesOf(const void* data, std::size_t size)
{
    auto bytes = Vector<std::uint8_t>((int) size);
    std::memcpy(bytes.data(), data, size);
    return bytes;
}

// Every name the runtime may look this source's binaries up by.
Vector<std::string> resourceNamesFor(const ShaderSource& source);

// What a build would embed for the source, or nothing where this machine
// cannot compile it: macOS without the Metal Toolchain, which since Xcode 26 is
// a separate download.
Resources compileAheadOfTime(const ShaderSource& source);
} // namespace eacp::GPU::PrecompiledTestBlobs
