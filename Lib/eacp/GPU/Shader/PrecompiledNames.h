#pragma once

#include "../Spirv/SpirvCompiler.h"
#include "ShaderSource.h"

#include <eacp/Core/Utils/Hash.h>

#include <string>
#include <string_view>

// How a precompiled shader blob is named: <keyFor(source)><suffix>, under
// resourceCategory. Inline and free of ResEmbed, so the build-time compiler
// (eacp-shader-compiler, linking only eacp-gpu-codegen) names what it writes
// through the very functions the runtime (PrecompiledShaders.h) looks it up by.
namespace eacp::GPU::Precompiled
{
inline constexpr std::string_view resourceCategory = "EacpShaders";

inline constexpr std::string_view macMetalSuffix = ".metallib";
inline constexpr std::string_view iosMetalSuffix = ".ios.metallib";
inline constexpr std::string_view iosSimulatorMetalSuffix = ".iossim.metallib";

inline constexpr std::string_view vertexDxbcSuffix = ".vs.dxbc";
inline constexpr std::string_view pixelDxbcSuffix = ".ps.dxbc";
inline constexpr std::string_view computeDxbcSuffix = ".cs.dxbc";

// The emitted text is the whole identity: two sources that hash alike share a
// binary, and any change to the emitter changes every key it touches.
inline std::string keyFor(std::string_view sourceText)
{
    return Hashing::toHex(Hashing::fnv1a64(sourceText));
}

inline std::string keyFor(const ShaderSource& source)
{
    return keyFor(std::string_view {source.source});
}

inline std::string resourceName(std::string_view key, std::string_view suffix)
{
    auto name = std::string(key);
    name += suffix;
    return name;
}

constexpr std::string_view spirvStageSuffix(Spirv::Stage stage)
{
    switch (stage)
    {
        case Spirv::Stage::Vertex:
            return ".vert.spv";
        case Spirv::Stage::Fragment:
            return ".frag.spv";
        case Spirv::Stage::Compute:
            return ".comp.spv";
    }

    return {};
}

// <key>.<target>.<stage>.spv, e.g. "0123456789abcdef.vk13spv16.frag.spv".
inline std::string
    spirvResourceName(std::string_view key, Spirv::Target target, Spirv::Stage stage)
{
    auto name = resourceName(key, ".");
    name += Spirv::targetName(target);
    name += spirvStageSuffix(stage);
    return name;
}
} // namespace eacp::GPU::Precompiled
