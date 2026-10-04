#include "ShaderSource.h"

namespace eacp::GPU
{
namespace
{
ShaderSource makeShaderSourceFor(ShaderBackend backend, std::string sourceToUse)
{
    auto result = ShaderSource {};
    result.backend = backend;
    result.source = std::move(sourceToUse);
    return result;
}
} // namespace

ShaderSource ShaderSource::msl(std::string sourceToUse)
{
    return makeShaderSourceFor(ShaderBackend::Metal, std::move(sourceToUse));
}

ShaderSource ShaderSource::hlsl(std::string sourceToUse)
{
    return makeShaderSourceFor(ShaderBackend::DirectX, std::move(sourceToUse));
}

ShaderSource ShaderSource::glsl(std::string sourceToUse)
{
    return makeShaderSourceFor(ShaderBackend::Vulkan, std::move(sourceToUse));
}

ShaderSource& ShaderSource::withVertex(std::string entry)
{
    vertexEntry = std::move(entry);
    return *this;
}

ShaderSource& ShaderSource::withFragment(std::string entry)
{
    fragmentEntry = std::move(entry);
    return *this;
}

ShaderSource& ShaderSource::withCompute(std::string entry)
{
    computeEntry = std::move(entry);
    return *this;
}

bool ShaderSource::isCompute() const
{
    return !computeEntry.empty();
}

ShaderSource& ShaderSource::withBinding(ResourceBinding binding)
{
    bindings.add(std::move(binding));
    return *this;
}
} // namespace eacp::GPU
