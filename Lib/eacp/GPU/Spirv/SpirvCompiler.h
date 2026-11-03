#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace eacp::GPU::Spirv
{
enum class Stage
{
    Vertex,
    Fragment,
    Compute
};

enum class Target
{
    vulkan11Spirv13,
    vulkan11Spirv14,
    vulkan13Spirv16
};

// The target as a precompiled shader's resource name spells it. Inline, so a
// build where eacp-spirv is not built can still name one.
constexpr std::string_view targetName(Target target)
{
    switch (target)
    {
        case Target::vulkan11Spirv13:
            return "vk11spv13";
        case Target::vulkan11Spirv14:
            return "vk11spv14";
        case Target::vulkan13Spirv16:
            return "vk13spv16";
    }

    return {};
}

// The log is empty on a clean compile.
struct CompileResult
{
    bool succeeded() const;

    Vector<uint32_t> words;
    std::string log;
};

// One stage of a whole GLSL 450 source, for `target`. Defines
// EACP_VERTEX or EACP_FRAGMENT ahead of it; the entry point is always main.
CompileResult compileGlsl(Stage stage,
                          const std::string& source,
                          Target target = Target::vulkan13Spirv16);

// Builds glslang's symbol tables, a one-time 90 ms the first compileGlsl would
// otherwise pay. Idempotent and thread-safe.
void warmUp();
} // namespace eacp::GPU::Spirv
