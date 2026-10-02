#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <cstdint>
#include <string>

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

// The log is empty on a clean compile.
struct CompileResult
{
    bool succeeded() const { return !words.empty(); }

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
