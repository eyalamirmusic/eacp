#include "CodegenCommon.h"

// The lowering pass turns the one dialect the emitter speaks - Vulkan GLSL 450
// - into the GLSL a GL context takes, and every rule it follows is pinned here
// on small hand-written sources and on an emitted one. Device-free: the sources
// are text in and text out, and where eacp-spirv is built CodegenCommon's
// checks hand each of them to glslang for all four targets as well.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
using Profile = GlslTarget::Profile;

constexpr auto core330 = GlslTarget {Profile::Core, 330};
constexpr auto core430 = GlslTarget {Profile::Core, 430};
constexpr auto es300 = GlslTarget {Profile::ES, 300};
constexpr auto es310 = GlslTarget {Profile::ES, 310};

// The shape the emitter gives a render shader: the shared declarations, then
// one #ifdef'd half per stage.
const char* renderSource = R"(#version 450

layout(std140, set = 0, binding = 0) uniform Uniforms
{
    float u0;
} uniforms;

layout(set = 0, binding = 8) uniform sampler2D texture0;

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;
layout(location = 0) out vec2 vary0;

void main()
{
    gl_Position = vec4(attr0, 0.0, 1.0);
    vary0 = attr0;
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec2 vary0;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = texture(texture0, vary0) * uniforms.u0;
}
#endif
)";

const char* kernelSource = R"(#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint count;
} uniforms;

layout(std430, set = 0, binding = 0) readonly buffer Buffer0
{
    float buffer0[];
};

layout(set = 0, binding = 24) uniform writeonly image2D texture0;

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

void main()
{
    uint gid = gl_GlobalInvocationID.x;

    if (gid < uniforms.count)
        imageStore(texture0, ivec2(int(gid), 0), vec4(buffer0[gid]));
}
)";

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

int bindingOf(const LoweredGlsl& lowered, const std::string& name)
{
    for (const auto& binding: lowered.bindings)
        if (binding.name == name)
            return binding.binding;

    return -1;
}

std::string lowered(const char* source, GlslTarget target, ShaderStage stage)
{
    const auto result = lowerGlsl(source, target, stage);
    check(result.succeeded(), result.error);

    return result.source;
}
} // namespace

// Every version the backend may meet, spelled the way the profile spells it.
auto tVersionLine = test("GlslLowering/theVersionLineIsRewritten") = []
{
    for (auto version: {330, 400, 430, 460})
    {
        auto source =
            lowered(renderSource, {Profile::Core, version}, ShaderStage::Fragment);

        check(source.starts_with("#version " + std::to_string(version) + " core\n"));
        check(!contains(source, "#version 450\n"));
    }

    for (auto version: {300, 310, 320})
    {
        auto source =
            lowered(renderSource, {Profile::ES, version}, ShaderStage::Fragment);

        check(source.starts_with("#version " + std::to_string(version) + " es\n"));
    }
};

// ES has no default precision for float in a fragment stage, and none for an
// image anywhere; core has one for everything.
auto tPrecision = test("GlslLowering/onlyESDeclaresPrecision") = []
{
    auto es = lowered(renderSource, es300, ShaderStage::Fragment);

    check(contains(es, "precision highp float;\nprecision highp int;\n"));
    check(!contains(es, "precision highp image2D;"));

    check(!contains(lowered(renderSource, core330, ShaderStage::Fragment),
                    "precision"));

    check(contains(lowered(kernelSource, es310, ShaderStage::Compute),
                   "precision highp image2D;"));
};

// A stage is compiled on its own, so the half the source hides behind the
// macro the emitter wrote is the half it defines.
auto tStageMacro = test("GlslLowering/theStageIsDefined") = []
{
    check(contains(lowered(renderSource, core330, ShaderStage::Vertex),
                   "#define EACP_VERTEX 1"));

    check(contains(lowered(renderSource, core330, ShaderStage::Fragment),
                   "#define EACP_FRAGMENT 1"));

    check(!contains(lowered(kernelSource, core430, ShaderStage::Compute),
                    "#define EACP_"));
};

// GL has one descriptor set and no way to say so.
auto tDescriptorSet = test("GlslLowering/theDescriptorSetIsDropped") = []
{
    for (auto target: {core330, core430, es300, es310})
    {
        auto stage =
            target.allowsCompute() ? ShaderStage::Compute : ShaderStage::Fragment;
        auto source = lowered(target.allowsCompute() ? kernelSource : renderSource,
                              target,
                              stage);

        check(!contains(source, "set = 0"));
        check(!contains(source, "set ="));
    }
};

// Core took layout(binding) in 420 and ES in 310. Below those the binding is
// the linked program's to answer, so the pass takes it out of the layout.
auto tBindingsPerTarget = test("GlslLowering/aBindingIsKeptOnlyWhereItIsLegal") = []
{
    auto newer = lowered(renderSource, core430, ShaderStage::Fragment);

    check(contains(newer, "layout(std140, binding = 0) uniform Uniforms"));
    check(contains(newer, "layout(binding = 8) uniform sampler2D texture0;"));

    check(contains(lowered(renderSource, es310, ShaderStage::Fragment),
                   "layout(binding = 8) uniform sampler2D texture0;"));

    for (auto target: {core330, es300})
    {
        auto source = lowered(renderSource, target, ShaderStage::Fragment);

        check(!contains(source, "binding"));
        check(contains(source, "layout(std140) uniform Uniforms"));

        // The layout that said nothing else goes with it.
        check(contains(source, "uniform sampler2D texture0;"));
        check(!contains(source, "layout() uniform sampler2D"));
    }
};

// Whatever the target let the layout keep, the list is complete: the GL
// ShaderLibrary binds every one of these by name after linking.
auto tRecordedBindings = test("GlslLowering/everyBindingIsRecordedByName") = []
{
    for (auto target: {core330, core430})
    {
        auto render = lowerGlsl(renderSource, target, ShaderStage::Fragment);

        check(render.bindings.size() == 2);
        check(bindingOf(render, "Uniforms") == 0);
        check(bindingOf(render, "texture0") == 8);
    }

    auto kernel = lowerGlsl(kernelSource, core430, ShaderStage::Compute);

    check(kernel.bindings.size() == 3);
    check(bindingOf(kernel, "Uniforms") == 16);
    check(bindingOf(kernel, "Buffer0") == 0);
    check(bindingOf(kernel, "texture0") == 24);
};

// A location on a vertex input or a fragment output is 3.3-legal and is what
// the vertex layout is matched by; on a varying it is 4.1, and the two halves
// match by name instead.
auto tLocations = test("GlslLowering/onlyAVaryingLosesItsLocation") = []
{
    auto vertex = lowered(renderSource, core330, ShaderStage::Vertex);

    check(contains(vertex, "layout(location = 0) in vec2 attr0;"));
    check(contains(vertex, "\nout vec2 vary0;"));

    auto fragment = lowered(renderSource, core330, ShaderStage::Fragment);

    check(contains(fragment, "layout(location = 0) out vec4 fragColor;"));
    check(contains(fragment, "\nin vec2 vary0;"));

    // The rule is the stage's, not the version's: nothing about a varying gets
    // better higher up.
    check(contains(lowered(renderSource, core430, ShaderStage::Vertex),
                   "\nout vec2 vary0;"));
};

// An image never says its format and never needs one: imageStore is all a
// kernel does with it.
auto tWritableImage = test("GlslLowering/aWritableImageIsLeftAlone") = []
{
    check(contains(lowered(kernelSource, core430, ShaderStage::Compute),
                   "layout(binding = 24) uniform writeonly image2D texture0;"));

    check(contains(lowered(kernelSource, es310, ShaderStage::Compute),
                   "writeonly image2D texture0;"));
};

// GL rasterizes NDC +1 into the highest row of the target where eacp puts the
// top of the picture, so a vertex stage ends with the sign the pass hands it.
auto tClipYWrapper = test("GlslLowering/aVertexStageIsWrappedForTheYFlip") = []
{
    auto vertex = lowered(renderSource, core330, ShaderStage::Vertex);

    check(contains(vertex, "void eacpMain()"));
    check(contains(vertex, "uniform float eacpClipYSign;"));
    check(contains(vertex, "    eacpMain();\n    gl_Position.y *= eacpClipYSign;"));
    check(vertex.find("void eacpMain()") < vertex.find("eacpMain();"));

    // The wrapper is the only thing the pass injects, so neither other stage
    // has one.
    auto fragment = lowered(renderSource, core330, ShaderStage::Fragment);

    check(contains(fragment, "void main()"));
    check(!contains(fragment, "eacpClipYSign"));
    check(!contains(lowered(kernelSource, core430, ShaderStage::Compute),
                    "eacpClipYSign"));
};

// Brace initializers are core 420 and have never been ES, so a constant array
// is declared the way every version reads it.
auto tArrayInitializer = test("GlslLowering/aConstantArrayIsConstructed") = []
{
    const auto* source = R"(#version 450

#ifdef EACP_VERTEX
layout(location = 0) in vec2 attr0;

void main()
{
    gl_Position = vec4(attr0, 0.0, 1.0);
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) out vec4 fragColor;

void main()
{
    vec3 a0[2] = {vec3(0.1, 0.1, 0.2), vec3(0.9, 0.4, 0.2)};
    fragColor = vec4(a0[1], 1.0);
}
#endif
)";

    auto fragment = lowered(source, es300, ShaderStage::Fragment);

    check(contains(fragment,
                   "vec3 a0[2] = vec3[2](vec3(0.1, 0.1, 0.2), "
                   "vec3(0.9, 0.4, 0.2));"));
    check(!contains(fragment, "= {"));

    expectGlslCompiles(source);
};

// A target too old for a source is not a source the pass failed to read, and
// the caller can tell the two apart.
auto tOlderTargets = test("GlslLowering/anOldTargetSaysSoRatherThanFailing") = []
{
    for (auto target: {core330, es300})
    {
        auto kernel = lowerGlsl(kernelSource, target, ShaderStage::Compute);

        check(!kernel.succeeded());
        check(kernel.needsNewerTarget);
        check(!kernel.error.empty());
    }

    check(lowerGlsl(kernelSource, core430, ShaderStage::Compute).succeeded());
    check(lowerGlsl(kernelSource, es310, ShaderStage::Compute).succeeded());
};

// What GL has no spelling for fails where the pipeline is built, not as a
// wrong picture at the draw.
auto tRefusals = test("GlslLowering/aVulkanOnlyConstructIsRefused") = []
{
    const auto* pushConstants = R"(#version 450

layout(push_constant) uniform Push
{
    float scale;
} push;

#ifdef EACP_VERTEX
void main()
{
    gl_Position = vec4(push.scale);
}
#endif
)";

    const auto* vertexIndex = R"(#version 450

#ifdef EACP_VERTEX
void main()
{
    gl_Position = vec4(float(gl_VertexIndex));
}
#endif
)";

    const auto* subgroupFold = R"(#version 450

layout(local_size_x = 64) in;

void main()
{
    float total = subgroupAdd(1.0);
}
)";

    for (const auto* source: {pushConstants, vertexIndex})
    {
        auto result = lowerGlsl(source, core430, ShaderStage::Vertex);

        check(!result.succeeded());
        check(!result.needsNewerTarget);
        check(!result.error.empty());
    }

    check(!lowerGlsl(subgroupFold, core430, ShaderStage::Compute).succeeded());

    // Not the dialect the emitter writes, so not one the pass may guess at.
    check(!lowerGlsl("#version 310 es\nvoid main() {}\n",
                     core430,
                     ShaderStage::Vertex)
               .succeeded());
};

// The wrapper has to find the entry to rename it.
auto tMissingEntry = test("GlslLowering/aVertexStageWithoutAnEntryIsRefused") = []
{
    const auto* noEntry = R"(#version 450

#ifdef EACP_VERTEX
void main ()
{
    gl_Position = vec4(0.0);
}
#endif
)";

    auto result = lowerGlsl(noEntry, core330, ShaderStage::Vertex);

    check(!result.succeeded());
    check(!result.needsNewerTarget);

    // The fragment stage never looks for it.
    check(lowerGlsl(noEntry, core330, ShaderStage::Fragment).succeeded());
};

// The emitter's own output, so a change to either side that parts them shows
// up here rather than on the one lane with a GL device.
auto tEmittedShader = test("GlslLowering/anEmittedShaderLowersForEveryTarget") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto carried = builder.varying(uv);
    auto image = builder.texture();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(sample(image, carried));

    auto source = emitGlsl(builder.graph());

    for (auto target: {core330, core430, es300, es310})
    {
        auto vertex = lowerGlsl(source, target, ShaderStage::Vertex);
        auto fragment = lowerGlsl(source, target, ShaderStage::Fragment);

        check(vertex.succeeded(), vertex.error);
        check(fragment.succeeded(), fragment.error);

        check(!contains(vertex.source, "set = 0"));
        check(contains(vertex.source, "layout(location = 0) in vec2 attr0;"));
        check(contains(vertex.source, "eacpClipYSign"));
        check(!contains(fragment.source, "eacpClipYSign"));

        check(bindingOf(fragment, "texture0") == vulkanTextureBinding(0));
        check(contains(fragment.source, "uniform sampler2D texture0;"));
    }

    expectGlslCompiles(builder.graph());
};
