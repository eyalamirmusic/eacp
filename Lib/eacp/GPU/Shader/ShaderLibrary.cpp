#include "ShaderLibrary.h"

namespace eacp::GPU
{
const std::string& ShaderLibrary::vertexEntry() const
{
    return vertexEntryName;
}

const std::string& ShaderLibrary::fragmentEntry() const
{
    return fragmentEntryName;
}

const std::string& ShaderLibrary::computeEntry() const
{
    return computeEntryName;
}

bool ShaderLibrary::hasFragment() const
{
    return !fragmentEntryName.empty();
}

ThreadGroupShape ShaderLibrary::threadGroupShape() const
{
    return groupShape;
}
} // namespace eacp::GPU
