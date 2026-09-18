#pragma once

#include "../Shader/ShaderSource.h"

#include <eacp/Core/Utils/Containers.h>

#include <string>
#include <string_view>

namespace eacp::GPU
{
// The GLSL a GL context accepts. The emitter speaks one dialect - Vulkan GLSL
// 450 - and lowerGlsl turns it into this one.
struct GlslTarget
{
    enum class Profile
    {
        Core,
        ES
    };

    bool isES() const { return profile == Profile::ES; }

    // layout(binding = N) on a block, a sampler or an image. Below these the
    // binding is a link-time question, which is what LoweredGlsl::bindings
    // answers.
    bool allowsExplicitBindings() const
    {
        return isES() ? version >= 310 : version >= 420;
    }

    bool allowsStorageBuffers() const
    {
        return isES() ? version >= 310 : version >= 430;
    }

    bool allowsImageStore() const
    {
        return isES() ? version >= 310 : version >= 420;
    }

    bool allowsCompute() const { return allowsStorageBuffers(); }

    // packHalf2x16 and its inverse, which the packed-vertex helpers call.
    bool allowsHalfPacking() const
    {
        return isES() ? version >= 300 : version >= 420;
    }

    Profile profile = Profile::Core;
    int version = 330; // 330, 400, 430, 460; 300, 310, 320 for ES
};

// A uniform block, storage block, sampler or image by the name the linked
// program knows it by, and the binding the Vulkan source named for it.
struct NamedBinding
{
    std::string name;
    int binding = 0;
};

// The lowered source, or the reason there is none. A caller reports a failure
// where it builds the pipeline, the way a failed SPIR-V compile is reported.
struct LoweredGlsl
{
    bool succeeded() const { return !source.empty(); }

    std::string source;

    // Every binding the source named, whether or not the target let the
    // layout() keep it: the GL ShaderLibrary binds these by name after
    // linking, which costs nothing and is the only route below core 420 /
    // ES 310.
    Vector<NamedBinding> bindings;

    // Empty on success.
    std::string error;

    // True when the source is fine but the target is too old for it - a kernel
    // on GL 3.3, a storage buffer on ES 3.0. Tells "this device cannot run this
    // shader" from "this source is not eacp's GLSL".
    bool needsNewerTarget = false;
};

// Rewrites one stage of an emitted (or hand-written) Vulkan GLSL 450 source
// for a GL target: the #version line, the ES precision lines, the stage macro
// the two-stage sources are written against, set = 0, the bindings the target
// cannot spell, the locations only a varying may not keep, and - for a vertex
// stage - the y-flip wrapper around main (GPU/README, plan.md D7).
LoweredGlsl
    lowerGlsl(std::string_view vulkanGlsl, GlslTarget target, ShaderStage stage);
} // namespace eacp::GPU
