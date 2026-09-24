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

// The log is empty on a clean compile.
struct CompileResult
{
    bool succeeded() const { return !words.empty(); }

    Vector<uint32_t> words;
    std::string log;
};

// One stage of a whole GLSL 450 source, for Vulkan 1.3 / SPIR-V 1.6. Defines
// EACP_VERTEX or EACP_FRAGMENT ahead of it; the entry point is always main.
CompileResult compileGlsl(Stage stage, const std::string& source);

// What produces the words compileGlsl returns: glslang's version and the target
// it compiles for. Changes whenever the same source could compile differently,
// so it is what a cache of those words is keyed by.
std::string compilerIdentity();

// Builds glslang's symbol tables, a one-time 90 ms the first compileGlsl would
// otherwise pay. Idempotent and thread-safe.
void warmUp();
} // namespace eacp::GPU::Spirv
