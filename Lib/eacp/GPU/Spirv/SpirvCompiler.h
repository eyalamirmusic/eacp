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

// The SPIR-V module a GLSL source compiled to, or the reason it did not. A
// clean compile leaves the log empty, so a caller can forward it unconditionally
// and only ever see something when there is something to see.
struct CompileResult
{
    bool succeeded() const { return !words.empty(); }

    Vector<uint32_t> words;
    std::string log;
};

// Compiles one stage of a GLSL 450 source for Vulkan 1.3 / SPIR-V 1.6. The
// source is the whole shader, first line `#version 450`: this defines
// EACP_VERTEX or EACP_FRAGMENT ahead of it so the vertex and fragment halves of
// one pipeline come out of one string and cannot drift. A compute source gets
// no macro. The entry point is always main.
CompileResult compileGlsl(Stage stage, const std::string& source);

// Builds glslang's built-in symbol tables, a one-time cost -- 90ms in a Release
// build, a few times that in Debug -- that otherwise lands on whichever
// compileGlsl happens to be first, which is a frame if nobody moved it.
// Idempotent and safe to call from any thread.
void warmUp();
} // namespace eacp::GPU::Spirv
