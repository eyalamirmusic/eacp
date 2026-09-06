#include "CodegenCommon.h"

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
// The triangle shader, authored in pure C++ via the EDSL. Mirrors the
// TriangleGen demo so the tests cover the exact path an app takes.
GeneratedShader makeTriangleShader()
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float3>();
    auto varyingColor = builder.varying(color);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(varyingColor, 1.0f));

    return builder.build();
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Derives the MSL uniform-block declaration string from the runtime constant
// that the emitter uses (RenderPass::uniformBase / ComputePass::uniformBase).
// Bumping the constant flows into both the emitter's output and the tests'
// expectation - one source of truth, no drift.
std::string uniformDecl(int base)
{
    return "constant Uniforms& uniforms [[buffer(" + std::to_string(base) + ")]]";
}

int countOccurrences(const std::string& haystack, const std::string& needle)
{
    auto count = 0;

    for (auto found = haystack.find(needle); found != std::string::npos;
         found = haystack.find(needle, found + needle.size()))
        ++count;

    return count;
}

// One graph in one dialect, with the few things that dialect spells its own way
// beside it - so a check that holds in all three is still written once. There
// are exactly three of those: the type vocabulary (GLSL says vec4 where the
// other two say float4), how a stage reads its own input (a struct member on
// the two that pass one, a global on GLSL, where `input` is a reserved word),
// and what marks a stage off (a second entry point, or the macro guarding the
// single main()).
struct Dialect
{
    std::string source;
    bool glsl = false;

    const char* spell(ValueType type) const
    {
        return glsl ? glslTypeName(type) : typeName(type);
    }

    std::string varying(int index) const
    {
        return (glsl ? "vary" : "input.v") + std::to_string(index);
    }

    std::string attribute(int index) const
    {
        return (glsl ? "attr" : "input.a") + std::to_string(index);
    }

    std::string fragmentStage() const
    {
        return glsl ? "#ifdef EACP_FRAGMENT" : "fragmentMain";
    }
};

Vector<Dialect> everyDialect(const ShaderGraph& graph)
{
    auto dialects = Vector<Dialect> {};
    dialects.add(Dialect {emitMetal(graph), false});
    dialects.add(Dialect {emitHlsl(graph), false});
    dialects.add(Dialect {emitGlsl(graph), true});
    return dialects;
}
} // namespace

// The generated vertex layout is derived from the same input declarations that
// produce the shader source, so it cannot drift from the shader. Pure logic, no
// GPU device required.
auto tCodegenLayout = test("GPU/codegenVertexLayout") = []
{
    auto shader = makeTriangleShader();
    const auto& layout = shader.vertexLayout;

    check(layout.attributes.size() == 2);
    check(layout.attributes[0].format == VertexFormat::Float2);
    check(layout.attributes[0].offset == 0);
    check(layout.attributes[1].format == VertexFormat::Float3);
    check(layout.attributes[1].offset == (int) (sizeof(float) * 2));
    check(layout.stride == (int) (sizeof(float) * 5));

    check(shader.source.vertexEntry == "vertexMain");
    check(shader.source.fragmentEntry == "fragmentMain");
};

// Which dialect build() hands back is the one thing about the codegen layer that
// is not the same on every host: ShaderBuilder-Apple/-Windows/-Linux.cpp is the
// single selection point, and picking the wrong one is invisible in the emitted
// text - every dialect is generated everywhere - until a pipeline is handed a
// source its compiler does not speak.
auto tCodegenNativeSourceBackend = test("GPU/codegenNativeSourceBackend") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float3>();
    auto varyingColor = builder.varying(color);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(varyingColor, 1.0f));

    auto shader = builder.build();
    const auto& graph = builder.graph();

    if constexpr (Platform::isLinux())
    {
        check(shader.source.backend == ShaderBackend::Vulkan);
        check(shader.source.source == emitGlsl(graph));
    }
    else if constexpr (Platform::isWindows())
    {
        check(shader.source.backend == ShaderBackend::DirectX);
        check(shader.source.source == emitHlsl(graph));
    }
    else
    {
        check(shader.source.backend == ShaderBackend::Metal);
        check(shader.source.source == emitMetal(graph));
    }

    expectGlslCompiles(graph);
};

// One IR emits both backends; assert each carries its backend-specific binding
// syntax. Pure string generation, runs on any host.
auto tCodegenEmitsBothBackends = test("GPU/codegenEmitsBothBackends") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float3>();
    auto varyingColor = builder.varying(color);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(varyingColor, 1.0f));

    auto metal = emitMetal(builder.graph());
    auto hlsl = emitHlsl(builder.graph());

    check(contains(metal, "[[attribute(0)]]"));
    check(contains(metal, "[[position]]"));
    check(contains(metal, "vertex VertexOut vertexMain"));
    check(contains(metal, "fragment float4 fragmentMain"));

    check(contains(hlsl, "TEXCOORD0"));
    check(contains(hlsl, "SV_Position"));
    check(contains(hlsl, "SV_Target"));

    // GLSL carries both stages in one string, told apart by the macro the
    // compiler defines, and names each piece of stage I/O rather than passing a
    // struct: attribute i at location i, varying i at location i on both sides
    // of the interface, the position in gl_Position and the colour in a
    // declared output.
    auto glsl = emitGlsl(builder.graph());
    check(glsl.rfind("#version 450\n", 0) == 0);
    check(contains(glsl, "#ifdef EACP_VERTEX\n"));
    check(contains(glsl, "#ifdef EACP_FRAGMENT\n"));
    check(countOccurrences(glsl, "void main()") == 2);
    check(countOccurrences(glsl, "#endif") == 2);
    check(contains(glsl, "layout(location = 0) in vec2 attr0;"));
    check(contains(glsl, "layout(location = 1) in vec3 attr1;"));
    check(contains(glsl, "layout(location = 0) out vec3 vary0;"));
    check(contains(glsl, "layout(location = 0) in vec3 vary0;"));
    check(contains(glsl, "layout(location = 0) out vec4 fragColor;"));
    check(contains(glsl, "    gl_Position = vec4(attr0, 0.0, 1.0);"));
    check(contains(glsl, "    fragColor = vec4(vary0, 1.0);"));

    // Neither reserved word is used as an identifier, which is what the two
    // renames above exist for.
    check(!contains(glsl, "input"));
    check(!contains(glsl, "output"));

    expectGlslCompiles(builder.graph());
};

// The stage-macro contract in full: every declaration a stage owns sits inside
// that stage's #ifdef and every declaration both stages share sits outside both,
// with the vertex block ahead of the fragment one. It is a contract rather than
// a formatting preference - the compiler wrapper builds the same string twice,
// defining one macro each time, so anything on the wrong side of a guard either
// goes missing from the stage that needed it or is declared twice in the stage
// that did not.
auto tCodegenGlslStagePartition = test("GPU/codegenGlslStagePartition") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto image = builder.texture();
    auto data = builder.inputBuffer();
    auto scale = builder.uniform<Float>();
    auto record = builder.uniform<UInt>();
    auto varyingUv = builder.varying(uv);

    builder.position(float4(position * scale, 0.0f, 1.0f));
    builder.fragment(sample(image, varyingUv) * data[record]);

    auto glsl = emitGlsl(builder.graph());

    auto vertexAt = glsl.find("#ifdef EACP_VERTEX");
    auto fragmentAt = glsl.find("#ifdef EACP_FRAGMENT");

    check(vertexAt != std::string::npos);
    check(vertexAt < fragmentAt);

    // Shared by both stages, so above either guard: the uniform block, the
    // combined image sampler, the storage block.
    check(glsl.find("uniform Uniforms") < vertexAt);
    check(glsl.find("uniform sampler2D texture0;") < vertexAt);
    check(glsl.find("buffer Buffer0") < vertexAt);

    // Owned by one stage, so inside its guard.
    check(vertexAt < glsl.find("in vec2 attr0;"));
    check(glsl.find("in vec2 attr0;") < fragmentAt);
    check(vertexAt < glsl.find("out vec2 vary0;"));
    check(glsl.find("out vec2 vary0;") < fragmentAt);
    check(fragmentAt < glsl.find("in vec2 vary0;"));
    check(fragmentAt < glsl.find("out vec4 fragColor;"));

    // A varying is an out in the vertex block and an in in the fragment block
    // at the one location, which is what the two halves of the interface match
    // on.
    check(countOccurrences(glsl, "layout(location = 0) out vec2 vary0;") == 1);
    check(countOccurrences(glsl, "layout(location = 0) in vec2 vary0;") == 1);

    expectGlslCompiles(builder.graph());
};

// Only the vertex stage is handed the vertex attributes. A fragment expression
// that reads one anyway is promoted to a varying - the vertex stage writes the
// attribute into it, the fragment stage reads it back - which is what the
// shader would have said had it declared the varying itself. Without that every
// dialect names an identifier the stage does not have: `input.a0` off a
// VertexOut with no such member on MSL and HLSL, `attr0` outside the block that
// declares it on GLSL.
auto tCodegenPromotedAttribute = test("GPU/codegenPromotedAttributeVarying") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto tint = builder.vertexInput<Float3>();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(tint, position.x()));

    for (const auto& dialect: everyDialect(builder.graph()))
    {
        auto fragment =
            dialect.source.substr(dialect.source.find(dialect.fragmentStage()));

        // Promoted in declaration order, whatever order the fragment expression
        // reached them in: attribute 0 takes varying 0, attribute 1 varying 1.
        check(contains(fragment,
                       "(" + dialect.varying(1) + ", (" + dialect.varying(0)
                           + ").x)"));

        // And nothing past the stage boundary names an attribute, which is the
        // whole of what this fixes.
        check(!contains(fragment, dialect.attribute(0)));
        check(!contains(fragment, dialect.attribute(1)));
    }

    // The declaration and the write, dialect by dialect.
    auto metal = emitMetal(builder.graph());
    check(contains(metal, "    float2 v0;\n    float3 v1;\n"));
    check(contains(metal, "    output.v0 = input.a0;\n    output.v1 = input.a1;\n"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl,
                   "    float2 v0 : TEXCOORD0;\n"
                   "    float3 v1 : TEXCOORD1;\n"));
    check(contains(hlsl, "    output.v0 = input.a0;\n    output.v1 = input.a1;\n"));

    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl,
                   "layout(location = 0) out vec2 vary0;\n"
                   "layout(location = 1) out vec3 vary1;\n"));
    check(contains(glsl,
                   "layout(location = 0) in vec2 vary0;\n"
                   "layout(location = 1) in vec3 vary1;\n"));
    check(contains(glsl, "    vary0 = attr0;\n    vary1 = attr1;\n"));

    expectGlslCompiles(builder.graph());
};

// A promoted varying is only ever one the shader did not already write. An
// attribute a declared varying already carries is read through that one, and
// the implicit varyings take the locations after every declared one - so
// reading an attribute from the fragment stage never moves a location the
// vertex/fragment interface already matched on.
auto tCodegenPromotedAttributeReuse =
    test("GPU/codegenPromotedAttributeReusesVarying") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto declared = builder.varying(uv);

    builder.position(float4(position, 0.0f, 1.0f));

    // uv is read as the attribute rather than through `declared`; position has
    // no varying at all.
    builder.fragment(float4(uv, position.x(), declared.y()));

    auto glsl = emitGlsl(builder.graph());

    // Two varyings, not three: the declared one carrying uv, and one promoted
    // for position after it.
    check(contains(glsl,
                   "layout(location = 0) out vec2 vary0;\n"
                   "layout(location = 1) out vec2 vary1;\n"));
    check(!contains(glsl, "vary2"));
    check(contains(glsl, "    vary0 = attr1;\n    vary1 = attr0;\n"));
    check(contains(glsl, "    fragColor = vec4(vary0, (vary1).x, (vary0).y);\n"));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "    float2 v0;\n    float2 v1;\n"));
    check(!contains(metal, "output.v2"));
    check(contains(metal, "    output.v0 = input.a1;\n    output.v1 = input.a0;\n"));

    check(contains(emitHlsl(builder.graph()),
                   "    float2 v0 : TEXCOORD0;\n"
                   "    float2 v1 : TEXCOORD1;\n"));

    expectGlslCompiles(builder.graph());
};

// There is nothing to interpolate between two integers, and none of the three
// dialects picks for you: GLSL rejects a non-flat integer stage input outright,
// HLSL wants nointerpolation and MSL wants [[flat]]. A float varying carries
// none of the three.
auto tCodegenFlatIntegerVarying = test("GPU/codegenFlatIntegerVarying") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto tile = builder.varying(toInt(position.x() * 8.0f));
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(toFloat(tile) * 0.125f, carried, 1.0f));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "    int v0 [[flat]];\n"));
    check(contains(metal, "    float2 v1;\n"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "    nointerpolation int v0 : TEXCOORD0;\n"));
    check(contains(hlsl, "    float2 v1 : TEXCOORD1;\n"));

    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "layout(location = 0) flat out int vary0;\n"));
    check(contains(glsl, "layout(location = 0) flat in int vary0;\n"));
    check(contains(glsl, "layout(location = 1) out vec2 vary1;\n"));
    check(!contains(glsl, "flat out vec2"));

    expectGlslCompiles(builder.graph());
};

// A kernel has one entry point, so it carries no macro at all - and its
// thread-group shape is a layout qualifier on the input rather than an
// attribute on the function.
auto tCodegenGlslComputeHasNoStageMacro =
    test("GPU/codegenGlslComputeHasNoStageMacro") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.outputBuffer();
    auto gid = builder.threadId();

    builder.write(output, gid, toFloat(gid));

    auto glsl = emitGlsl(builder.graph());
    check(glsl.rfind("#version 450\n", 0) == 0);
    check(!contains(glsl, "#ifdef"));
    check(!contains(glsl, "#endif"));
    check(countOccurrences(glsl, "void main()") == 1);
    check(contains(glsl,
                   "layout(local_size_x = "
                       + std::to_string(ComputePass::threadGroupWidth)
                       + ", local_size_y = 1, local_size_z = 1) in;"));
    check(contains(glsl, "    uint gid = gl_GlobalInvocationID.x;"));

    auto grid = ShaderBuilder {};
    auto gridOutput = grid.outputBuffer();
    auto p = grid.threadPosition();
    grid.write(gridOutput, p.y * 16u + p.x, toFloat(p.x));

    auto square = std::to_string(ComputePass::threadGroupSize2D);
    auto twoD = emitGlsl(grid.graph());
    check(contains(twoD,
                   "layout(local_size_x = " + square + ", local_size_y = " + square
                       + ", local_size_z = 1) in;"));
    check(contains(twoD, "    uvec2 gid = gl_GlobalInvocationID.xy;"));

    expectGlslCompiles(builder.graph());
    expectGlslCompiles(grid.graph());
};

// Every binding number in the emitted source comes from the one binding map, so
// a shader and the descriptor set layout the C++ binder builds cannot disagree.
// Written from the constants rather than from literals: move one and the
// expectation moves with it, where a test naming a number would only say where
// it used to be.
auto tCodegenGlslBindings = test("GPU/codegenGlslBindings") = []
{
    auto render = ShaderBuilder {};

    auto position = render.vertexInput<Float2>();
    auto uv = render.vertexInput<Float2>();
    auto first = render.texture();
    auto second = render.texture();
    auto palette = render.inputBuffer();
    auto tint = render.uniform<Float4>();
    auto record = render.uniform<UInt>();
    auto varyingUv = render.varying(uv);

    render.position(float4(position, 0.0f, 1.0f));
    render.fragment(sample(first, varyingUv) * sample(second, varyingUv) * tint
                    * palette[record]);

    auto glsl = emitGlsl(render.graph());

    check(contains(glsl,
                   "layout(std140, set = 0, binding = "
                       + std::to_string(vulkanUniformBinding)
                       + ") uniform Uniforms"));

    for (auto slot = 0; slot < 2; ++slot)
        check(contains(
            glsl,
            "layout(set = 0, binding = " + std::to_string(vulkanTextureBinding(slot))
                + ") uniform sampler2D texture" + std::to_string(slot) + ";"));

    check(contains(glsl,
                   "layout(std430, set = 0, binding = "
                       + std::to_string(vulkanBufferBinding(0))
                       + ") readonly buffer"));

    // A combined image sampler is the whole declaration: the sampler is
    // immutable in the pipeline layout, built from the slot's TextureSampling,
    // so nothing about sampling reaches the source at all.
    check(!contains(glsl, "samplerConfig"));

    // A kernel binds at the Metal indices instead: buffers from zero, textures
    // above every buffer slot, the block above both.
    auto compute = ShaderBuilder {};

    auto input = compute.inputBuffer();
    auto output = compute.outputBuffer();
    auto image = compute.texture();
    auto target = compute.writableTexture();
    auto p = compute.threadPosition();

    compute.write(target,
                  p.x,
                  p.y,
                  sample(image, float2(toFloat(p.x), toFloat(p.y))) * input[p.x]);
    compute.write(output, p.x, toFloat(p.y));

    auto kernel = emitGlsl(compute.graph());

    check(contains(kernel,
                   "layout(std140, set = 0, binding = "
                       + std::to_string(vulkanComputeUniformBinding)
                       + ") uniform Uniforms"));
    check(contains(kernel,
                   "layout(std430, set = 0, binding = "
                       + std::to_string(vulkanComputeBufferBinding(0))
                       + ") readonly buffer Buffer0"));
    check(contains(kernel,
                   "layout(std430, set = 0, binding = "
                       + std::to_string(vulkanComputeBufferBinding(1))
                       + ") buffer Buffer1"));
    check(contains(kernel,
                   "layout(set = 0, binding = "
                       + std::to_string(vulkanComputeTextureBinding(0))
                       + ") uniform sampler2D texture0;"));
    check(contains(kernel,
                   "layout(set = 0, binding = "
                       + std::to_string(vulkanComputeTextureBinding(1))
                       + ") uniform writeonly image2D texture1;"));

    // The two ranges cannot overlap, which is the whole of what one descriptor
    // set costs over Metal's separate index spaces.
    check(vulkanTextureBinding(maxTextureSlots - 1) < vulkanBufferBinding(0));

    expectGlslCompiles(render.graph());
    expectGlslCompiles(compute.graph());
};

// A uniform<>() declaration adds a uniform block bound per-frame, and sin/cos
// are emitted as builtin calls. Built inline (like codegenEmitsBothBackends) so
// both backends can be inspected headlessly. Pure string generation.
auto tCodegenUniformEmits = test("GPU/codegenUniformEmits") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float3>();
    auto angle = builder.uniform<Float>();
    auto varyingColor = builder.varying(color);

    auto c = cos(angle);
    auto s = sin(angle);
    auto px = position.x();
    auto py = position.y();
    builder.position(float4(float2(px * c - py * s, px * s + py * c), 0.0f, 1.0f));
    builder.fragment(float4(varyingColor, 1.0f));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "struct Uniforms"));
    check(contains(metal, uniformDecl(RenderPass::uniformBase)));
    check(contains(metal, "cos(uniforms.u0)"));
    check(contains(metal, "sin(uniforms.u0)"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "cbuffer UniformsCB : register(b0)"));
    check(contains(hlsl, "cos(uniforms.u0)"));
    check(contains(hlsl, "sin(uniforms.u0)"));

    // The GLSL block is an interface block with an instance name, which is what
    // keeps "uniforms.u0" the one spelling all three dialects read it by.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl,
                   "layout(std140, set = 0, binding = "
                       + std::to_string(vulkanUniformBinding)
                       + ") uniform Uniforms\n{\n    float u0;\n} uniforms;"));
    check(contains(glsl, "cos(uniforms.u0)"));
    check(contains(glsl, "sin(uniforms.u0)"));

    // The plain triangle declares no uniforms, so no block is emitted.
    auto plain = makeTriangleShader();
    check(!contains(plain.source.source, "Uniforms"));

    expectGlslCompiles(builder.graph());
};

// HLSL cbuffer packing only forbids straddling a 16-byte register, while the
// CPU block follows MSL struct alignment (a vec3 aligns to 16). A scalar
// followed by a vector is where they disagree: HLSL would pack the float3 at
// offset 4, the CPU writes it at 16, so the emitter must pad the cbuffer
// struct up to the CPU offsets.
auto tCodegenCbufferPadding = test("GPU/codegenHlslCbufferPadding") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto brightness = builder.uniform<Float>();
    auto tint = builder.uniform<Float3>();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(tint * brightness, 1.0f));

    // One pad moves the float3 to offset 8, where the no-straddle rule bumps it
    // the rest of the way to 16; the emitter pads minimally and lets the rule
    // finish the job.
    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl,
                   "    float u0;\n"
                   "    float pad0;\n"
                   "    float3 u1;\n"));

    // MSL aligns the vec3 to 16 natively, so its struct needs no padding.
    auto metal = emitMetal(builder.graph());
    check(!contains(metal, "pad"));

    // std140 aligns it to 16 too, so the GLSL block needs none either: this is
    // the shape HLSL alone gets wrong.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "    float u0;\n    vec3 u1;\n"));
    check(!contains(glsl, "pad"));

    expectGlslCompiles(builder.graph());
};

// The one shape std140 disagrees with the CPU block about, and the reason the
// GLSL arm needs a pad pass of its own rather than none: std140 gives a vec3
// twelve bytes where MSL gives it a full sixteen, so a scalar after one packs
// four bytes low unless a pad scalar takes the gap. It is the mirror image of
// the HLSL case above - there a vector after a scalar is what slips.
auto tCodegenStd140Padding = test("GPU/codegenGlslStd140Padding") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto tint = builder.uniform<Float3>();
    auto fade = builder.uniform<Float>();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(tint * fade, 1.0f));

    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl,
                   "    vec3 u0;\n"
                   "    float pad0;\n"
                   "    float u1;\n"));

    // MSL packs the same bytes natively, so it pads nothing. HLSL pads here as
    // well, for its own reason - its vec3 is twelve bytes too - which is why
    // this is the one shape the two non-native arms agree on.
    check(!contains(emitMetal(builder.graph()), "pad"));
    check(contains(emitHlsl(builder.graph()),
                   "    float3 u0;\n"
                   "    float pad0;\n"
                   "    float u1;\n"));

    // And the pad is exactly the gap the CPU block leaves: the scalar is
    // written at 16, which is where the shader now reads it.
    auto types = Vector<ValueType> {};
    types.add(ValueType::Float3);
    types.add(ValueType::Float);

    auto offsets = uniformOffsets(types);
    check(offsets[0] == 0);
    check(offsets[1] == 16);
    check(std140PackedOffset(byteSize(ValueType::Float3), ValueType::Float) == 12);
    check(std140PackedOffset(16, ValueType::Float) == 16);

    expectGlslCompiles(builder.graph());
};

// Where the two layouts disagree about the block rather than about a field:
// std140 gives a uniform block a base alignment of sixteen, where the CPU stops
// at its widest member's. Every field stays where it was - this is a tail pad -
// but the Vulkan backend has to size its UNIFORM_BUFFER_DYNAMIC range with the
// rounded number, a range shorter than the block the shader declares being a
// validation error.
auto tCodegenStd140BlockSize = test("GPU/codegenGlslStd140BlockSize") = []
{
    auto typesOf = [](std::initializer_list<ValueType> list)
    {
        auto types = Vector<ValueType> {};

        for (auto type: list)
            types.add(type);

        return types;
    };

    // Two scalars: the CPU block ends at eight, std140 rounds it to sixteen.
    // This is the shape the rounding exists for.
    check(uniformBlockSize(typesOf({ValueType::Float, ValueType::Float})) == 8);
    check(std140BlockSize(typesOf({ValueType::Float, ValueType::Float})) == 16);

    // A vec2 after a scalar ends on the boundary already, so both agree.
    check(uniformBlockSize(typesOf({ValueType::Float, ValueType::Float2})) == 16);
    check(std140BlockSize(typesOf({ValueType::Float, ValueType::Float2})) == 16);

    // A vec3 takes a full sixteen either side of the boundary, so the scalar
    // after it ends the block at twenty and both round the same way - the CPU
    // to its widest member's sixteen, std140 to its own.
    check(uniformBlockSize(typesOf({ValueType::Float3, ValueType::Float})) == 32);
    check(std140BlockSize(typesOf({ValueType::Float3, ValueType::Float})) == 32);

    check(uniformBlockSize(typesOf({ValueType::Float4x4, ValueType::Float})) == 80);
    check(std140BlockSize(typesOf({ValueType::Float4x4, ValueType::Float})) == 80);

    // A shader with no uniforms declares no block, and neither size invents one.
    check(uniformBlockSize(Vector<ValueType> {}) == 0);
    check(std140BlockSize(Vector<ValueType> {}) == 0);

    // And the CPU size is what the field walk already arrives at: the last
    // field's end, rounded up to the widest field's alignment.
    auto types = typesOf({ValueType::Float2, ValueType::Float4});
    auto offsets = uniformOffsets(types);
    check(offsets[1] == 16);
    check(uniformBlockSize(types) == 32);
    check(std140BlockSize(types) == 32);
};

// A float2 after a float packs at 4 in HLSL but at 8 on the CPU side, so it
// pads by one scalar; vector-only blocks land identically under both rule sets
// and stay pad-free.
auto tCodegenCbufferPaddingFloat2 = test("GPU/codegenHlslCbufferPaddingFloat2") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto brightness = builder.uniform<Float>();
    auto offset = builder.uniform<Float2>();

    builder.position(float4(position + offset, 0.0f, 1.0f));
    builder.fragment(
        float4(position.x(), position.y(), brightness, builder.constant(1.0f)));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl,
                   "    float u0;\n"
                   "    float pad0;\n"
                   "    float2 u1;\n"));

    // std140 aligns a vec2 to 8 the way MSL does, so its block needs no pad.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "    float u0;\n    vec2 u1;\n"));
    check(!contains(glsl, "pad"));

    auto vectors = ShaderBuilder {};
    auto vectorPosition = vectors.vertexInput<Float2>();
    auto viewport = vectors.uniform<Float2>();
    auto color = vectors.uniform<Float4>();

    vectors.position(float4(vectorPosition + viewport, 0.0f, 1.0f));
    vectors.fragment(color);

    check(!contains(emitHlsl(vectors.graph()), "pad"));
    check(!contains(emitGlsl(vectors.graph()), "pad"));

    // The first graph reads a vertex attribute from the fragment stage with no
    // varying between them; the emitter promotes it to one, so both compile.
    // See codegenPromotedAttributeVarying.
    expectGlslCompiles(builder.graph());
    expectGlslCompiles(vectors.graph());
};

// Unary minus and float literal operands record IR nodes directly, with no
// constant() wrapping at the call site. Pure string generation.
auto tCodegenOperatorSugar = test("GPU/codegenOperatorSugar") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto x = position.x();

    builder.position(float4(float2(-x * 2.0f, 1.0f - x), 0.0f, 1.0f));
    builder.fragment(float4(float3(x, x, x), 1.0f));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "(-((input.a0).x))"));
    check(contains(metal, " * 2.0)"));
    check(contains(metal, "(1.0 - (input.a0).x)"));

    expectGlslCompiles(builder.graph());
};

// Negating a negative constant emits nested parentheses, not a pre-decrement:
// rotateX(-72 degrees) bakes sin() as a negative literal and then negates it.
auto tCodegenNegatedNegative = test("GPU/codegenNegatedNegativeConstant") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto negated = -builder.constant(-0.5f);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(float2(negated, negated), float2(negated, negated)));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "(-(-0.5))"));
    check(!contains(metal, "--"));

    expectGlslCompiles(builder.graph());
};

// An operation referenced more than once is hoisted into a named local and
// computed once per stage, so generated source stays linear in the graph size
// instead of re-inlining shared subtrees at every use. Leaf reads stay inline.
auto tCodegenSharedSubexpressions = test("GPU/codegenSharedSubexpressions") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float3>();
    auto angle = builder.uniform<Float>();
    auto varyingColor = builder.varying(color);

    // cos/sin each feed both rotated components: one local each in the vertex
    // stage.
    auto c = cos(angle);
    auto s = sin(angle);
    auto px = position.x();
    auto py = position.y();
    builder.position(float4(float2(px * c - py * s, px * s + py * c), 0.0f, 1.0f));

    // normalize() feeds both the colour and its scale: one local in the
    // fragment stage.
    auto unit = normalize(varyingColor);
    builder.fragment(float4(unit * length(unit), 1.0f));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "    float t0 = cos(uniforms.u0);\n"));
    check(contains(metal, "    float t1 = sin(uniforms.u0);\n"));
    check(countOccurrences(metal, "cos(") == 1);
    check(countOccurrences(metal, "sin(") == 1);

    check(contains(metal, "    float3 t0 = normalize(input.v0);\n"));
    check(countOccurrences(metal, "normalize(") == 1);
    check(contains(metal, "length(t0)"));

    auto hlsl = emitHlsl(builder.graph());
    check(countOccurrences(hlsl, "cos(") == 1);
    check(countOccurrences(hlsl, "sin(") == 1);
    check(countOccurrences(hlsl, "normalize(") == 1);

    expectGlslCompiles(builder.graph());
};

// Intrinsics carry the canonical MSL name and translate where HLSL spells
// differently: fract -> frac, mix -> lerp; the rest are shared. Pure string
// generation.
auto tCodegenIntrinsicNames = test("GPU/codegenIntrinsicNames") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float3>();
    auto varyingColor = builder.varying(color);

    builder.position(float4(position, 0.0f, 1.0f));

    auto t = fract(varyingColor.x());
    auto shaped = smoothstep(0.0f, 1.0f, t);
    auto tinted = mix(varyingColor, normalize(varyingColor), shaped);
    auto lit = clamp(tinted * abs(varyingColor.y()), 0.0f, 1.0f);
    builder.fragment(float4(lit, 1.0f));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "fract("));
    check(contains(metal, "mix("));
    check(contains(metal, "smoothstep(0.0, 1.0, "));
    check(contains(metal, "clamp("));
    check(contains(metal, "abs("));
    check(contains(metal, "normalize("));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "frac("));
    check(contains(hlsl, "lerp("));
    check(contains(hlsl, "smoothstep("));
    check(!contains(hlsl, "fract("));
    check(!contains(hlsl, "mix("));

    // The canonical names are GLSL's own, so this arm renames none of these -
    // it is HLSL that is the odd one out on fract and mix.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "fract("));
    check(contains(glsl, "mix("));
    check(contains(glsl, "smoothstep(0.0, 1.0, "));
    check(contains(glsl, "clamp("));
    check(!contains(glsl, "frac(("));
    check(!contains(glsl, "lerp("));

    expectGlslCompiles(builder.graph());
};

// A literal wherever the language takes one. Every intrinsic used to come in
// two shapes - one where every argument is a handle, and one where the scalar
// arguments are all literals - and a shader mixes them freely: smoothstep(0.0,
// w, d) has one edge of each, min(0.0, g) puts the literal first, step(d, 0.0)
// puts it second, and mix(0.5, 1.0, h) interpolates between two constants by
// something computed. All of them are legal GLSL and all of them have a
// spelling in both languages under this, so which positions take a literal is
// not something the EDSL should have an opinion about.
//
// What this pins is the position: an anchored literal has to record where it
// was written, since every one of these means something else if it moves.
auto tCodegenLiteralArguments = test("GPU/codegenLiteralArguments") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto width = builder.uniform<Float>();

    builder.position(float4(position, 0.0f, 1.0f));

    auto carried = builder.varying(position);

    auto edge = smoothstep(0.0f, width, length(carried));
    auto lowest = min(0.0f, carried.x());
    auto gate = step(carried.y(), 0.0f);
    auto curve = pow(2.0f, width);
    auto blend = mix(0.5f, 1.0f, edge);
    auto held = clamp(carried.x() + carried.y(), 0.0f, width);
    auto raised = max(-1.0f, carried.y());

    // clamp was the last one still insisting on a handle in front, which no
    // reading of the module would have turned up: it took a corpus, and one
    // shader in it writing clamp(0.02, 2.0, t), which is an odd thing to write
    // and is what GLSL says a shader may.
    auto pinned = clamp(0.02f, 2.0f, width);
    auto bounded = clamp(0.25f, carried.x(), width);

    builder.fragment(
        float4(lowest + gate + pinned, curve * blend, held + bounded, raised));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "smoothstep(0.0, uniforms.u0, "));
    check(contains(metal, "min(0.0, "));
    check(contains(metal, "step((input.v0).y, 0.0)"));
    check(contains(metal, "pow(2.0, uniforms.u0)"));
    check(contains(metal, "mix(0.5, 1.0, "));
    check(contains(metal, ", 0.0, uniforms.u0)"));
    check(contains(metal, "max(-1.0, "));
    check(contains(metal, "clamp(0.02, 2.0, uniforms.u0)"));
    check(contains(metal, "clamp(0.25, (input.v0).x, uniforms.u0)"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "lerp(0.5, 1.0, "));
    check(contains(hlsl, "step((input.v0).y, 0.0)"));
    check(contains(hlsl, "clamp(0.02, 2.0, uniforms.u0)"));

    expectGlslCompiles(builder.graph());
};

// The transcendental, rounding and geometric intrinsics all spell identically
// in both backends; only the screen-space derivatives differ, dfdx/dfdy against
// HLSL's ddx/ddy. Pure string generation.
auto tCodegenTranscendentalNames = test("GPU/codegenTranscendentalNames") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto normal = builder.vertexInput<Float3>();
    auto carried = builder.varying(normal);

    builder.position(float4(position, 0.0f, 1.0f));

    auto angle = atan2(carried.y(), carried.x());
    auto swept = tan(asin(acos(atan(angle))));
    auto falloff = exp(-log(exp2(log2(swept))) * rsqrt(sign(swept) + 2.0f));
    auto edged = ceil(trunc(round(falloff))) + fwidth(falloff) + dfdx(falloff)
                 + dfdy(falloff);
    auto bounced = reflect(carried, normalize(carried))
                   + refract(carried, normalize(carried), 0.5f)
                   + faceforward(carried, carried, carried);

    builder.fragment(float4(bounced * edged, distance(carried, bounced)));

    auto metal = emitMetal(builder.graph());

    for (const auto* name:
         {"atan2(",   "tan(",     "asin(",        "acos(",   "exp(",
          "exp2(",    "log(",     "log2(",        "rsqrt(",  "sign(",
          "ceil(",    "trunc(",   "round(",       "fwidth(", "distance(",
          "reflect(", "refract(", "faceforward(", "dfdx(",   "dfdy("})
        check(contains(metal, name));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "ddx("));
    check(contains(hlsl, "ddy("));
    check(!contains(hlsl, "dfdx("));
    check(!contains(hlsl, "dfdy("));

    // GLSL renames four of them and shares the rest: the two-argument arctangent
    // is spelled atan there, the reciprocal square root inversesqrt, and the two
    // derivatives carry a capital F.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "dFdx("));
    check(contains(glsl, "dFdy("));
    check(contains(glsl, "inversesqrt("));
    check(!contains(glsl, "dfdx("));
    check(!contains(glsl, "dfdy("));
    check(!contains(glsl, "rsqrt("));
    check(!contains(glsl, "atan2("));

    for (const auto* name: {"tan(",
                            "asin(",
                            "acos(",
                            "atan(",
                            "exp(",
                            "exp2(",
                            "log(",
                            "log2(",
                            "sign(",
                            "ceil(",
                            "trunc(",
                            "round(",
                            "fwidth(",
                            "distance(",
                            "reflect(",
                            "refract(",
                            "faceforward("})
        check(contains(glsl, name));

    expectGlslCompiles(builder.graph());
};

// mod() is the floored modulus, recorded as x - y * floor(x / y) rather than as
// a call: the only modulus either backend offers is fmod(), which truncates, so
// every tile left of the origin would come out mirrored. Pure string
// generation.
auto tCodegenFlooredModulus = test("GPU/codegenFlooredModulus") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto scale = builder.uniform<Float>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(mod(carried, 2.0f), mod(carried.x(), scale), 1.0f));

    auto metal = emitMetal(builder.graph());

    check(contains(metal, "floor("));
    check(!contains(metal, "mod("));

    // The divisor scales the floored quotient before it is subtracted, and the
    // operand order is kept: neither half of this commutes.
    check(contains(metal, " - (2.0 * floor("));
    check(contains(metal, " / 2.0)"));

    expectGlslCompiles(builder.graph());
};

// Every ordering of two and three components has an accessor, so a coordinate
// swap is one Swizzle node rather than a constructor over rebuilt parts. Both
// backends spell the components identically. Pure string generation.
auto tCodegenSwizzleOrderings = test("GPU/codegenSwizzleOrderings") = []
{
    // An accessor is constrained to the widths that can name its components:
    // .zw belongs to a Float4 and is no part of a Float2, while .xxy widens a
    // Float2 the way the shading languages do. The rule is asserted rather than
    // probed with requires() - an unsatisfied constraint on a plain member
    // function is a hard error at the call, which is the diagnostic wanted, but
    // it leaves nothing for a requires-expression to fold to false.
    static_assert(detail::spellableAt(4, "zw"));
    static_assert(detail::spellableAt(2, "yx"));
    static_assert(detail::spellableAt(2, "xxy"));
    static_assert(detail::spellableAt(4, "zyxw"));
    static_assert(!detail::spellableAt(2, "zw"));
    static_assert(!detail::spellableAt(2, "xyz"));
    static_assert(!detail::spellableAt(3, "xyw"));

    static_assert(requires(Float4 value) { value.zw(); });
    static_assert(requires(Float2 value) { value.yx(); });
    static_assert(requires(Float2 value) { value.xxy(); });
    static_assert(requires(Float4 value) { value.zyxw(); });

    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float4>();
    auto carried = builder.varying(color);

    builder.position(float4(position.yx(), 0.0f, 1.0f));
    builder.fragment(float4(carried.zyx() + carried.wzy(), carried.zw().y())
                     + carried.zyxw());

    auto metal = emitMetal(builder.graph());
    check(contains(metal, ").yx"));
    check(contains(metal, ").zyx"));
    check(contains(metal, ").wzy"));
    check(contains(metal, ").zw"));
    check(contains(metal, ").zyxw"));

    // A four-component swizzle is one node: the source is read once, not
    // rebuilt from four extracted components.
    check(countOccurrences(metal, "input.v0") == 4);

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, ").zyx"));
    check(contains(hlsl, ").zyxw"));

    expectGlslCompiles(builder.graph());
};

// The 2x2 and 3x3 matrices follow the 4x4 in every respect that matters: built
// from columns, multiplied with the * operator on MSL and mul() on HLSL, and
// transposed at construction on HLSL, which fills a matrix from rows. Pure
// string generation.
auto tCodegenSmallMatrices = test("GPU/codegenSmallMatrices") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto normal = builder.vertexInput<Float3>();
    auto angle = builder.uniform<Float>();

    auto rotation =
        float2x2(float2(cos(angle), sin(angle)), float2(-sin(angle), cos(angle)));

    builder.position(float4(rotation * position, 0.0f, 1.0f));

    auto basis = float3x3(builder.varying(normal),
                          float3(0.0f, 1.0f, builder.constant(0.0f)),
                          float3(0.0f, builder.constant(0.0f), 1.0f));

    builder.fragment(float4(basis * (basis * builder.varying(normal)), 1.0f));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "float2x2("));
    check(contains(metal, "float3x3("));
    check(!contains(metal, "transpose("));
    check(!contains(metal, "mul("));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "transpose(float2x2("));
    check(contains(hlsl, "transpose(float3x3("));
    check(contains(hlsl, "mul("));

    // GLSL fills a matrix from columns as MSL does and multiplies with the
    // operator, so its output is the Metal one with the names changed - which is
    // the whole of why the GLSL arm is closer to MSL than HLSL is.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "mat2("));
    check(contains(glsl, "mat3("));
    check(!contains(glsl, "transpose("));
    check(!contains(glsl, "mul("));
    check(!contains(glsl, "float2x2("));
    check(!contains(glsl, "float3x3("));

    expectGlslCompiles(builder.graph());
};

// transpose() and determinant() are where the small matrices stop being
// write-only. Both backends spell both the same way, and both are right on both
// for the same reason: HLSL holds transposed what MSL holds, so transposing
// each leaves each holding the transpose of the same logical matrix, and a
// determinant is equal for a matrix and its transpose either way.
//
// The check that matters is the HLSL one, where the construction already
// emitted a transpose of its own: the two have to nest rather than cancel.
// Pure string generation.
auto tCodegenMatrixTranspose = test("GPU/codegenMatrixTranspose") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto normal = builder.vertexInput<Float3>();

    builder.position(float4(position, 0.0f, 1.0f));

    auto carried = builder.varying(normal);

    // Two matrices rather than one used twice, so neither construction is
    // promoted to a shared local and each call still has one under it to read.
    auto basis = float3x3(carried,
                          float3(0.0f, 1.0f, builder.constant(0.0f)),
                          float3(0.0f, builder.constant(0.0f), 1.0f));

    auto other = float3x3(float3(1.0f, builder.constant(0.0f), 0.0f),
                          carried,
                          float3(0.0f, builder.constant(0.0f), 1.0f));

    builder.fragment(float4(transpose(basis) * carried, determinant(other)));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "transpose(float3x3("));
    check(contains(metal, "determinant(float3x3("));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "transpose(transpose(float3x3("));
    check(contains(hlsl, "determinant(transpose(float3x3("));

    // GLSL has both builtins under the same names and needs no transpose at the
    // construction, so neither call nests.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "transpose(mat3("));
    check(contains(glsl, "determinant(mat3("));
    check(!contains(glsl, "transpose(transpose("));

    expectGlslCompiles(builder.graph());
};

// Comparisons yield a Bool, the connectives combine them, and select picks
// between two values without branching. Both backends spell all of it the same
// way, which is why none of these needs a per-backend form. Pure string
// generation.
auto tCodegenComparisons = test("GPU/codegenComparisons") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto threshold = builder.uniform<Float>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto inside = carried.x() < threshold && carried.y() >= 0.0f;
    auto edge = !(carried.x() == threshold);

    builder.fragment(float4(select(inside, 1.0f, 0.0f),
                            select(edge, carried.y(), threshold),
                            select(inside, carried.xy(), carried.yx())));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, " < uniforms.u0)"));
    check(contains(metal, " >= 0.0)"));
    check(contains(metal, " && "));
    check(contains(metal, "(!("));
    check(contains(metal, " == uniforms.u0)"));
    check(contains(metal, " ? 1.0 : 0.0)"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, " && "));
    check(contains(hlsl, " ? 1.0 : 0.0)"));

    expectGlslCompiles(builder.graph());
};

// A mutable local is a statement, not an expression: it is declared where it is
// created and every read after an assignment sees the assigned value. Pure
// string generation.
auto tCodegenMutableLocal = test("GPU/codegenMutableLocal") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto total = builder.var(0.0f);
    total += carried.x();
    total = total.get() * 2.0f;

    builder.fragment(float4(total, total, total, 1.0f));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "float v0 = 0.0;"));
    check(contains(metal, "v0 = (v0 + (input.v0).x);"));
    check(contains(metal, "v0 = (v0 * 2.0);"));
    check(contains(metal, "return float4(v0, v0, v0, 1.0);"));

    // The declaration comes before the assignments, which come before the
    // colour that reads them: statement order is recording order.
    check(metal.find("float v0 = 0.0;") < metal.find("v0 = (v0 + "));
    check(metal.find("v0 = (v0 * 2.0);") < metal.find("return float4(v0"));

    expectGlslCompiles(builder.graph());
};

// if / else, with each body scoped to its own braces. Pure string generation.
auto tCodegenBranches = test("GPU/codegenBranches") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto shade = builder.var(0.0f);

    builder.ifThen(
        carried.x() < 0.0f,
        [&] { shade = carried.y(); },
        [&]
        {
            auto inner = builder.var(1.0f);
            inner *= carried.y();
            shade = inner.get();
        });

    builder.fragment(float4(shade, shade, shade, 1.0f));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "if (((input.v0).x < 0.0))"));
    check(contains(metal, "\n    else\n"));

    // The else body declares a variable of its own, indented inside the block
    // that owns it.
    check(contains(metal, "\n        float v1 = 1.0;"));
    check(contains(emitHlsl(builder.graph()), "\n        float v1 = 1.0;"));

    expectGlslCompiles(builder.graph());
};

// A while loop, its two jumps, and the one rule the emitter cannot get wrong:
// the condition is printed into the header rather than bound to a local before
// it, or the loop would test a value that never changes. Pure string
// generation.
auto tCodegenLoop = test("GPU/codegenLoop") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto limit = builder.uniform<Float>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto travelled = builder.var(0.0f);
    auto steps = builder.var(0.0f);

    builder.loop(steps < 64.0f,
                 [&]
                 {
                     steps += 1.0f;

                     auto step = abs(carried.x()) + 0.01f;

                     builder.ifThen(step < 0.001f, [&] { builder.breakLoop(); });
                     builder.ifThen(travelled > limit,
                                    [&] { builder.continueLoop(); });

                     travelled += step;
                 });

    builder.fragment(float4(travelled, travelled, travelled, 1.0f));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "while ((v1 < 64.0))"));
    check(contains(metal, "            break;"));
    check(contains(metal, "            continue;"));

    // The condition reads the variable the body writes, so it must not have
    // been hoisted: no bool local is bound ahead of the loop.
    check(metal.find("while (") < metal.find("v1 = (v1 + 1.0);"));
    check(!contains(metal, "bool t"));

    check(contains(emitHlsl(builder.graph()), "while ((v1 < 64.0))"));

    expectGlslCompiles(builder.graph());
};

// A shared subtree inside a loop body is named there, not before the loop: a
// local defined outside would hold the value the first iteration computed for
// every one after it. Pure string generation.
auto tCodegenLoopLocalsStayInside = test("GPU/codegenLoopLocalsStayInside") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto total = builder.var(0.0f);

    builder.loop(total < 8.0f,
                 [&]
                 {
                     auto shared = sin(total.get() * carried.x());
                     total += shared * shared;
                 });

    builder.fragment(float4(total, total, total, 1.0f));

    auto metal = emitMetal(builder.graph());

    auto loopAt = metal.find("while (");
    auto localAt = metal.find("float t0 = sin(");

    check(localAt != std::string::npos);
    check(loopAt < localAt);
    check(countOccurrences(metal, "sin(") == 1);

    expectGlslCompiles(builder.graph());
};

// A value a body tests and then uses is computed once. The name spans the two
// statements, which is the shape every raymarcher has: measure the distance,
// stop if it is small enough, otherwise step by it. Pure string generation.
auto tCodegenSharedAcrossStatements = test("GPU/codegenSharedAcrossStatements") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto travelled = builder.var(0.0f);

    builder.loop(travelled < 10.0f,
                 [&]
                 {
                     auto distance =
                         length(float3(carried, 1.0f) * travelled.get()) - 1.0f;

                     builder.ifThen(distance < 0.001f, [&] { builder.breakLoop(); });

                     travelled += distance;
                 });

    builder.fragment(float4(travelled, travelled, travelled, 1.0f));

    auto metal = emitMetal(builder.graph());
    check(countOccurrences(metal, "length(") == 1);
    check(contains(metal, "if ((t0 < 0.001))"));
    check(contains(metal, "v0 = (v0 + t0);"));

    expectGlslCompiles(builder.graph());
};

// ...and a name is given up the moment a statement writes a variable the value
// behind it was computed from, which is the one thing sharing across statements
// can get wrong. Pure string generation.
auto tCodegenStaleLocalsAreDropped = test("GPU/codegenStaleLocalsAreDropped") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto total = builder.var(carried.x());
    auto shade = builder.var(0.0f);

    auto scaled = sin(total.get());

    shade = scaled + scaled;
    total = total.get() + 1.0f;
    shade = scaled * 2.0f + scaled;

    builder.fragment(float4(shade, shade, shade, 1.0f));

    auto metal = emitMetal(builder.graph());

    // Two names for the one expression: what stands for sin(v0) before v0 moves
    // cannot stand for it afterwards.
    check(countOccurrences(metal, "sin(v0)") == 2);
    check(contains(metal, "float t0 = sin(v0);"));
    check(contains(metal, "float t1 = sin(v0);"));
    check(metal.find("v0 = (v0 + 1.0);") < metal.find("float t1 = sin(v0);"));

    expectGlslCompiles(builder.graph());
};

// A vector times a matrix, which is the same product read against the matrix's
// rows rather than against its columns - what a shader writes to go back
// through an orientation rather than into one. Neither backend needs a form of
// its own for it: MSL's operator and HLSL's mul() both read whichever operand
// is on the left as a row vector, so the order the two are written in is the
// whole of what tells the three products apart. That order is what this pins,
// because the other one is a different value and compiles just as happily.
auto tCodegenVectorTimesMatrix = test("GPU/codegenVectorTimesMatrix") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto angle = builder.uniform<Float>();

    builder.position(float4(position, 0.0f, 1.0f));

    auto carried = builder.varying(position);

    // Two matrices rather than one used twice, so neither construction is
    // promoted to a shared local and each product still has one under it.
    auto into =
        float2x2(float2(cos(angle), sin(angle)), float2(-sin(angle), cos(angle)));

    auto back =
        float2x2(float2(cos(angle), -sin(angle)), float2(sin(angle), cos(angle)));

    builder.fragment(float4(into * carried, carried * back));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "(float2x2("));
    check(contains(metal, " * float2x2("));

    // On HLSL the construction is transposed and the product is a call, so the
    // matrix is mul()'s first argument in one and its second in the other.
    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "mul(transpose(float2x2("));
    check(contains(hlsl, ", transpose(float2x2("));

    // And GLSL keeps the order the way MSL does, the operator reading whichever
    // operand is on the left as a row.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "(mat2("));
    check(contains(glsl, " * mat2("));

    expectGlslCompiles(builder.graph());
};

// A uniform read only by the fragment expression binds the block to the
// fragment stage: the MSL fragment function gains the uniforms parameter and
// the vertex function drops it. The HLSL cbuffer is a global both stages
// already see. Pure string generation.
auto tCodegenFragmentUniformEmits = test("GPU/codegenFragmentUniformEmits") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.uniform<Float4>();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(color);

    auto metal = emitMetal(builder.graph());
    check(
        contains(metal, "vertex VertexOut vertexMain(VertexIn input [[stage_in]])"));
    check(contains(metal,
                   "fragment float4 fragmentMain(VertexOut input [[stage_in]],\n    "
                       + uniformDecl(RenderPass::uniformBase) + ")"));
    check(contains(metal, "return uniforms.u0;"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "cbuffer UniformsCB : register(b0)"));
    check(contains(hlsl, "return uniforms.u0;"));

    expectGlslCompiles(builder.graph());
};

// A uniform read by both stages puts the parameter on both Metal functions:
// one block, bound twice, one slot rule.
auto tCodegenSharedUniformEmits = test("GPU/codegenSharedUniformEmits") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto scale = builder.uniform<Float>();

    auto scaled = float2(position.x() * scale, position.y() * scale);
    builder.position(float4(scaled, 0.0f, 1.0f));
    builder.fragment(float4(scale, scale, scale, builder.constant(1.0f)));

    auto metal = emitMetal(builder.graph());
    check(contains(metal,
                   "vertex VertexOut vertexMain(VertexIn input [[stage_in]], "
                       + uniformDecl(RenderPass::uniformBase) + ")"));
    check(contains(metal,
                   "fragment float4 fragmentMain(VertexOut input [[stage_in]],\n    "
                       + uniformDecl(RenderPass::uniformBase) + ")"));

    expectGlslCompiles(builder.graph());
};

// GeneratedShader reports which stage reads a uniform, and it is the emitter's
// own answer rather than a second opinion: each flag is checked against whether
// the Metal signature beside it declared the block. That is what
// RenderPass::draw(program) binds from, so a bound stage and a declared
// parameter cannot drift apart. Pure string generation, no GPU device required.
auto tCodegenUniformStages = test("GPU/codegenUniformStages") = []
{
    // build() emits the host's backend, so the signature is read through
    // emitMetal for a platform-independent comparison; the flags come off the
    // graph and are the same either way.
    auto stagesOf = [](const ShaderGraph& graph, const GeneratedShader& generated)
    {
        auto metal = emitMetal(graph);
        auto declaration = uniformDecl(RenderPass::uniformBase);

        auto vertexDeclares = contains(
            metal, "vertexMain(VertexIn input [[stage_in]], " + declaration);
        auto fragmentDeclares = contains(
            metal, "fragmentMain(VertexOut input [[stage_in]],\n    " + declaration);

        // The flag and the signature are two statements of one fact; assert
        // they agree before reading either.
        check(generated.vertexReadsUniforms == vertexDeclares);
        check(generated.fragmentReadsUniforms == fragmentDeclares);
    };

    // Read only by the position: the fragment stage is never bound.
    auto vertexOnly = ShaderBuilder {};
    auto vertexPosition = vertexOnly.vertexInput<Float2>();
    auto scale = vertexOnly.uniform<Float>();
    vertexOnly.position(
        float4(vertexPosition.x() * scale, vertexPosition.y() * scale, 0.0f, 1.0f));
    vertexOnly.fragment(float4(vertexOnly.constant(1.0f),
                               vertexOnly.constant(1.0f),
                               vertexOnly.constant(1.0f),
                               vertexOnly.constant(1.0f)));

    auto vertexShader = vertexOnly.build();
    stagesOf(vertexOnly.graph(), vertexShader);
    check(vertexShader.vertexReadsUniforms);
    check(!vertexShader.fragmentReadsUniforms);

    // Read only by the colour: the vertex stage is never bound.
    auto fragmentOnly = ShaderBuilder {};
    auto fragmentPosition = fragmentOnly.vertexInput<Float2>();
    auto color = fragmentOnly.uniform<Float4>();
    fragmentOnly.position(float4(fragmentPosition, 0.0f, 1.0f));
    fragmentOnly.fragment(color);

    auto fragmentShader = fragmentOnly.build();
    stagesOf(fragmentOnly.graph(), fragmentShader);
    check(!fragmentShader.vertexReadsUniforms);
    check(fragmentShader.fragmentReadsUniforms);

    // Declared and read by neither stage: nothing is bound at all, though the
    // program still has a block to pack.
    auto unread = ShaderBuilder {};
    auto unreadPosition = unread.vertexInput<Float2>();
    auto unusedTint = unread.uniform<Float4>();
    (void) unusedTint;
    unread.position(float4(unreadPosition, 0.0f, 1.0f));
    unread.fragment(float4(unread.constant(1.0f),
                           unread.constant(0.0f),
                           unread.constant(0.0f),
                           unread.constant(1.0f)));

    auto unreadShader = unread.build();
    stagesOf(unread.graph(), unreadShader);
    check(!unreadShader.vertexReadsUniforms);
    check(!unreadShader.fragmentReadsUniforms);

    expectGlslCompiles(vertexOnly.graph());
    expectGlslCompiles(fragmentOnly.graph());
    expectGlslCompiles(unread.graph());
};

// A uniform read only from inside a statement still counts towards the fragment
// stage: the flag is collected over the statement roots, not over the colour
// expression alone. Without that a shader whose uniform is read only inside a
// loop or a branch would go unbound and read garbage - the same trap the
// declaration walk already guards against.
auto tCodegenUniformInStatementBinds =
    test("GPU/codegenUniformInStatementBinds") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);
    auto threshold = builder.uniform<Float>();

    builder.position(float4(position, 0.0f, 1.0f));

    auto shade = builder.var(0.0f);
    builder.ifThen(carried.x() > threshold, [&] { shade = threshold; });

    builder.fragment(float4(shade, shade, shade, 1.0f));

    auto generated = builder.build();
    check(!generated.vertexReadsUniforms);
    check(generated.fragmentReadsUniforms);
    check(contains(emitMetal(builder.graph()),
                   "fragmentMain(VertexOut input [[stage_in]],\n    "
                       + uniformDecl(RenderPass::uniformBase)));

    expectGlslCompiles(builder.graph());
};

// A texture() declaration reaches both backends with paired texture / sampler
// bindings at the same index: fragment function parameters on Metal, globals
// with t/s registers on D3D. Pure string generation.
auto tCodegenTextureEmits = test("GPU/codegenTextureEmits") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto image = builder.texture();
    auto varyingUv = builder.varying(uv);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(sample(image, varyingUv));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "texture2d<float> texture0 [[texture(0)]]"));
    check(contains(metal, "sampler sampler0 [[sampler(0)]]"));
    check(contains(metal, "texture0.sample(sampler0, input.v0)"));

    // The sampler is named for the sampling configuration, not for the
    // texture, and that is the whole of the difference between the two
    // backends' declarations: MSL passes a sampler per texture as a function
    // argument, HLSL binds one per configuration to a register. Default
    // sampling is Nearest/Clamp, which is configuration 0.
    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "Texture2D texture0 : register(t0);"));
    check(contains(hlsl, "SamplerState samplerConfig0 : register(s0);"));
    check(contains(hlsl, "texture0.Sample(samplerConfig0, input.v0)"));

    // The vertex stage carries no texture parameters; only the fragment
    // signature gains them on Metal.
    check(
        contains(metal, "vertex VertexOut vertexMain(VertexIn input [[stage_in]])"));

    // GLSL binds one combined image sampler, which is both declarations at once:
    // the sampler is immutable in the pipeline layout and built from the slot's
    // TextureSampling, so nothing about sampling reaches the source, and the
    // sample names only the texture.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl,
                   "layout(set = 0, binding = "
                       + std::to_string(vulkanTextureBinding(0))
                       + ") uniform sampler2D texture0;"));
    check(contains(glsl, "texture(texture0, vary0)"));
    check(!contains(glsl, "sampler0"));
    check(!contains(glsl, "samplerConfig"));

    expectGlslCompiles(builder.graph());
};

// A cubeTexture() declaration changes the declared type and nothing else. The
// sampler sits at the same register, the slot comes from the same counter - a
// cube declared beside a 2D image lands on slot 1 - and the sample is spelled
// exactly as the 2D one is, because both shading languages read both kinds
// through the same call and let the coordinate's width choose.
//
// That last part is the reason this is a test rather than a comment: it means
// ExprKind::Sample never asks what shape the texture is, so the only thing
// standing between a cube and a Texture2D in the generated source is these four
// declaration sites. Pure string generation.
auto tCodegenCubeTextureEmits = test("GPU/codegenCubeTextureEmits") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto normal = builder.vertexInput<Float3>();
    auto flat = builder.texture();
    auto cube = builder.cubeTexture();
    auto varyingUv = builder.varying(uv);
    auto varyingNormal = builder.varying(normal);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(sample(flat, varyingUv) * sample(cube, varyingNormal));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "texture2d<float> texture0 [[texture(0)]]"));
    check(contains(metal, "texturecube<float> texture1 [[texture(1)]]"));
    check(contains(metal, "sampler sampler1 [[sampler(1)]]"));
    check(contains(metal, "texture1.sample(sampler1, input.v1)"));

    // Both textures were declared with the same (default) sampling, so on
    // HLSL there is exactly one SamplerState between them and both samples go
    // through it. This is what keeps a texture slot from costing a sampler
    // register - there are 16 of those and rather more slots than that.
    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "Texture2D texture0 : register(t0);"));
    check(contains(hlsl, "TextureCube texture1 : register(t1);"));
    check(contains(hlsl, "SamplerState samplerConfig0 : register(s0);"));
    check(!contains(hlsl, "samplerConfig1"));
    check(contains(hlsl, "texture1.Sample(samplerConfig0, input.v1)"));

    // Not a Texture2DArray and not a texture2d_array: the cube's own type is the
    // one thing that has to be right, and both backends have a near neighbour
    // that would compile and sample nothing.
    check(!contains(hlsl, "Texture2DArray"));
    check(!contains(metal, "texture2d_array"));

    // The GLSL cube is a samplerCube at the slot's own binding, and its sample
    // is the same call the 2D one gets - the coordinate's width choosing, as on
    // the other two.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl,
                   "layout(set = 0, binding = "
                       + std::to_string(vulkanTextureBinding(0))
                       + ") uniform sampler2D texture0;"));
    check(contains(glsl,
                   "layout(set = 0, binding = "
                       + std::to_string(vulkanTextureBinding(1))
                       + ") uniform samplerCube texture1;"));
    check(contains(glsl, "texture(texture1, vary1)"));
    check(!contains(glsl, "sampler2DArray"));

    expectGlslCompiles(builder.graph());
};

// Choosing the mip level is where the two backends stop agreeing on the call:
// Metal passes it to the same sample(), HLSL has a method of its own for it.
// One graph node, so a shader says it once. Pure string generation.
auto tCodegenSampleLevelEmits = test("GPU/codegenSampleLevelEmits") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto image = builder.texture();
    auto varyingUv = builder.varying(uv);
    auto level = builder.uniform<Float>();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(sample(image, varyingUv, level));

    auto metal = emitMetal(builder.graph());
    check(
        contains(metal, "texture0.sample(sampler0, input.v0, level(uniforms.u0))"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl,
                   "texture0.SampleLevel(samplerConfig0, input.v0, uniforms.u0)"));

    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "textureLod(texture0, vary0, uniforms.u0)"));
    check(!contains(glsl, "texture(texture0"));

    expectGlslCompiles(builder.graph());
};

// A level given as a plain float needs no anchoring by the caller: the texture
// already carries the graph the constant records into. Reading the top of the
// pyramid is what most shaders that pick a level at all are asking for, so it
// would otherwise be the one call that needs a ShaderBuilder in scope.
auto tCodegenLiteralSampleLevel = test("GPU/codegenLiteralSampleLevel") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto image = builder.texture();
    auto varyingUv = builder.varying(uv);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(sample(image, varyingUv, 0.0f));

    check(contains(emitMetal(builder.graph()),
                   "texture0.sample(sampler0, input.v0, level(0.0))"));

    check(contains(emitHlsl(builder.graph()),
                   "texture0.SampleLevel(samplerConfig0, input.v0, 0.0)"));

    expectGlslCompiles(builder.graph());
};

// A texel read takes no sampler at all, and the coordinate goes through int2 on
// both backends: Metal reads unsigned, so a negative coordinate has to become a
// large one there rather than an undefined conversion, which is what makes it
// read as zero on both. Pure string generation.
auto tCodegenFetchEmits = test("GPU/codegenFetchEmits") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto image = builder.texture();
    auto varyingUv = builder.varying(uv);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(fetch(image, varyingUv));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "texture0.read(uint2(int2(input.v0)))"));
    check(!contains(metal, "texture0.sample"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "texture0.Load(int3(int2(input.v0), 0))"));
    check(!contains(hlsl, "texture0.Sample"));

    // GLSL takes the coordinate signed like D3D's Load, and the level as an
    // argument of its own: texelFetch has no derivatives to pick one from, so
    // omitting it is not a default but a compile error.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "texelFetch(texture0, ivec2(vary0), 0)"));
    check(!contains(glsl, "texture(texture0"));

    expectGlslCompiles(builder.graph());
};

// A compute kernel authored via the EDSL: storage buffers, the thread id, a
// uniform and a store. The kernel scaffolding differs per backend (function
// parameters on Metal, globals + numthreads on D3D); the body and the implicit
// element-count guard are shared. Pure string generation.
auto tCodegenComputeEmits = test("GPU/codegenComputeEmits") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto scale = builder.uniform<Float>();
    auto gid = builder.threadId();

    builder.write(output, gid, input[gid] * scale);

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "kernel void computeMain("));
    check(contains(metal, "device const float* buffer0 [[buffer(0)]]"));
    check(contains(metal, "device float* buffer1 [[buffer(1)]]"));
    check(contains(metal, uniformDecl(ComputePass::uniformBase)));
    check(contains(metal, "uint gid [[thread_position_in_grid]]"));
    check(contains(metal, "uint count;"));
    check(contains(metal, "if (gid >= uniforms.count)"));
    check(contains(metal, "buffer1[gid] = (buffer0[gid] * uniforms.u0);"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "StructuredBuffer<float> buffer0 : register(t0);"));
    check(contains(hlsl, "RWStructuredBuffer<float> buffer1 : register(u1);"));
    check(contains(hlsl, "cbuffer UniformsCB : register(b0)"));
    check(contains(hlsl, "[numthreads(64, 1, 1)]"));
    check(contains(hlsl, "uint3 threadId : SV_DispatchThreadID"));
    check(contains(hlsl, "uint gid = threadId.x;"));
    check(contains(hlsl, "if (gid >= uniforms.count)"));
    check(contains(hlsl, "buffer1[gid] = (buffer0[gid] * uniforms.u0);"));

    // GLSL declares each buffer as a storage block with no instance name, so
    // its run of elements is a global and the body reads exactly as the other
    // two do. The ids come out of the builtins into the same names.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl,
                   "layout(std430, set = 0, binding = "
                       + std::to_string(vulkanComputeBufferBinding(0))
                       + ") readonly buffer Buffer0\n{\n    float buffer0[];\n};"));
    check(contains(glsl,
                   "layout(std430, set = 0, binding = "
                       + std::to_string(vulkanComputeBufferBinding(1))
                       + ") buffer Buffer1\n{\n    float buffer1[];\n};"));
    check(contains(glsl, "uint count;"));
    check(contains(glsl, "    uint gid = gl_GlobalInvocationID.x;"));
    check(contains(glsl, "if (gid >= uniforms.count)"));
    check(contains(glsl, "buffer1[gid] = (buffer0[gid] * uniforms.u0);"));

    expectGlslCompiles(builder.graph());
};

// A buffer element read more than once hoists into a named local like any
// other shared operation, and build() marks the source as compute with the
// kernel entry point and no vertex layout. A kernel without user uniforms
// still gets the block: the implicit count lives there.
auto tCodegenComputeSharedRead = test("GPU/codegenComputeSharedRead") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto gid = builder.threadId();

    auto value = input[gid];
    builder.write(output, gid, value * value);

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "struct Uniforms"));
    check(contains(metal, "uint count;"));
    check(contains(metal, "    float t0 = buffer0[gid];\n"));
    check(contains(metal, "buffer1[gid] = (t0 * t0);"));
    check(countOccurrences(metal, "buffer0[gid]") == 1);

    auto shader = builder.build();
    check(shader.source.isCompute());
    check(shader.source.computeEntry == "computeMain");
    check(shader.vertexLayout.attributes.size() == 0);

    expectGlslCompiles(builder.graph());
};

// Index arithmetic: uint operators against uint values and integer literals
// (recorded as uint constant nodes), a uint uniform read inside an index
// expression, and uint min/max. The spelling is shared by both backends.
auto tCodegenComputeIndexArithmetic = test("GPU/codegenComputeIndexArithmetic") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto length = builder.uniform<UInt>();
    auto gid = builder.threadId();

    auto previous = input[(gid + length - 1u) % length];
    auto next = input[min(gid + 1u, max(length, 1u) - 1u)];
    builder.write(output, gid * 2u, (previous + next) / 2.0f);

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint u0;"));
    check(contains(metal, "buffer0[(((gid + uniforms.u0) - 1u) % uniforms.u0)]"));
    check(contains(metal, "min((gid + 1u), (max(uniforms.u0, 1u) - 1u))"));
    check(contains(metal, "buffer1[(gid * 2u)] = "));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "uint u0;"));
    check(contains(hlsl, "buffer0[(((gid + uniforms.u0) - 1u) % uniforms.u0)]"));
    check(contains(hlsl, "buffer1[(gid * 2u)] = "));

    expectGlslCompiles(builder.graph());
};

// The uint comparisons, and the loop they unlock: a reduction kernel is a
// Var<UInt> counter tested against a uint bound - until these existed the
// counter had to be a float carried in lockstep beside the index. The literal
// forms record uint constant nodes, so the whole header spells in uints. Pure
// string generation.
auto tCodegenComputeUIntLoop = test("GPU/codegenComputeUIntLoop") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto count = builder.uniform<UInt>();
    auto gid = builder.threadId();

    auto total = builder.var(0.0f);
    auto i = builder.var(0u);

    builder.loop(i < count,
                 [&]
                 {
                     total += input[gid * count + i];
                     i += 1u;
                 });

    builder.ifThen(gid == 0u, [&] { total *= 2.0f; });

    builder.write(output, gid, total);

    for (const auto& dialect: everyDialect(builder.graph()))
    {
        const auto& source = dialect.source;

        check(contains(source, "uint v1 = 0u;"));
        check(contains(source, "while ((v1 < uniforms.u0))"));
        check(contains(source, "v1 = (v1 + 1u);"));
        check(contains(source, "if ((gid == 0u))"));

        // The condition reads the counter the body advances, so it must be
        // printed into the header rather than bound to a local before it.
        check(source.find("while (") < source.find("v1 = (v1 + 1u);"));
    }

    expectGlslCompiles(builder.graph());
};

// Crossing between the index vocabularies: into the signed one for arithmetic
// that may go below zero, back out with toUInt once clamped, and toUInt of a
// float scalar truncating towards zero. Constructor-style casts, spelled the
// same by both backends. Pure string generation.
auto tCodegenComputeIndexCasts = test("GPU/codegenComputeIndexCasts") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto scale = builder.uniform<Float>();
    auto gid = builder.threadId();

    auto previous = input[toUInt(max(toInt(gid) - 1, 0))];
    auto scaled = input[toUInt(toFloat(gid) * scale)];

    builder.write(output, gid, previous + scaled);

    // The three scalar casts spell the same in all three dialects, which is
    // what makes this one text: it is the *vector* casts that GLSL renames.
    for (const auto& dialect: everyDialect(builder.graph()))
    {
        check(contains(dialect.source, "uint(max((int(gid) - 1), 0))"));
        check(contains(dialect.source, "uint((float(gid) * uniforms.u0))"));
    }

    expectGlslCompiles(builder.graph());
};

// A 2D kernel: threadPosition() gives a pair of indices, which changes the
// entry signature, the threadgroup shape and the implicit extents the guard
// reads. Asserting the emitted signature rather than only the runtime result is
// the point - a rank that reached the dispatch but not the emitter would leave
// the kernel reading a thread id of the wrong shape and say nothing about it.
auto tCodegenCompute2D = test("GPU/codegenCompute2D") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.outputBuffer();
    auto position = builder.threadPosition();

    builder.write(output, position.y * 16u + position.x, toFloat(position.x));

    auto shader = builder.build();
    check(shader.dispatchRank == DispatchRank::TwoD);

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint2 gid [[thread_position_in_grid]]"));
    check(contains(metal, "uint width;"));
    check(contains(metal, "uint height;"));
    check(!contains(metal, "uint count;"));
    check(
        contains(metal, "if (gid.x >= uniforms.width || gid.y >= uniforms.height)"));
    check(contains(metal, "buffer0[((gid.y * 16u) + gid.x)] = float(gid.x);"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "[numthreads(8, 8, 1)]"));
    check(contains(hlsl, "uint3 threadId : SV_DispatchThreadID"));
    check(contains(hlsl, "uint2 gid = threadId.xy;"));
    check(
        contains(hlsl, "if (gid.x >= uniforms.width || gid.y >= uniforms.height)"));
    check(contains(hlsl, "buffer0[((gid.y * 16u) + gid.x)] = float(gid.x);"));

    auto glsl = emitGlsl(builder.graph());
    check(contains(
        glsl,
        "layout(local_size_x = " + std::to_string(ComputePass::threadGroupSize2D)
            + ", local_size_y = " + std::to_string(ComputePass::threadGroupSize2D)
            + ", local_size_z = 1) in;"));
    check(contains(glsl, "    uvec2 gid = gl_GlobalInvocationID.xy;"));
    check(
        contains(glsl, "if (gid.x >= uniforms.width || gid.y >= uniforms.height)"));
    check(contains(glsl, "buffer0[((gid.y * 16u) + gid.x)] = float(gid.x);"));

    expectGlslCompiles(builder.graph());
};

// A kernel that reads one texture and writes another. Read and written
// textures take slots from one counter, because Metal binds both to one texture
// index space; on D3D they land in the t and u spaces they share with the
// storage buffers, above every buffer slot. Pure string generation.
auto tCodegenComputeTextureWrite = test("GPU/codegenComputeTextureWrite") = []
{
    auto builder = ShaderBuilder {};

    auto source = builder.texture();
    auto target = builder.writableTexture();
    auto p = builder.threadPosition();

    builder.write(
        target, p.x, p.y, sample(source, float2(toFloat(p.x), toFloat(p.y))));

    auto shader = builder.build();
    check(shader.source.isCompute());
    check(shader.source.computeEntry == "computeMain");

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "texture2d<float> texture0 [[texture(0)]]"));
    check(contains(metal, "sampler sampler0 [[sampler(0)]]"));
    check(
        contains(metal, "texture2d<float, access::write> texture1 [[texture(1)]]"));

    // A written texture has no sampler on either backend: there is nothing to
    // sample it with and nothing to read out of it.
    check(!contains(metal, "sampler sampler1"));
    check(contains(metal,
                   "texture1.write(texture0.sample(sampler0, float2(float(gid.x), "
                   "float(gid.y))), uint2(gid.x, gid.y));"));

    // Written from ComputePass::textureRegisterBase rather than from a literal,
    // since the base is where the buffer slots end - move the one and the other
    // moves with it, and a test naming a number would only say where it used to
    // be.
    auto textureRegister = [](int slot)
    { return std::to_string(ComputePass::textureRegisterBase + slot); };

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl,
                   "Texture2D texture0 : register(t" + textureRegister(0) + ");"));
    check(contains(hlsl, "SamplerState samplerConfig0 : register(s0);"));
    check(contains(hlsl,
                   "RWTexture2D<float4> texture1 : register(u" + textureRegister(1)
                       + ");"));
    check(contains(hlsl,
                   "texture1[uint2(gid.x, gid.y)] = texture0.Sample(samplerConfig0, "
                   "float2(float(gid.x), float(gid.y)));"));

    // A written slot is a storage image on GLSL rather than a sampled one, and
    // its store takes a *signed* coordinate - the one thing that would compile
    // nowhere else and has to be got right here. No format layout qualifier
    // either: writeonly is what lets the declaration stay agnostic of the
    // format the texture was created in, as the other two are.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl,
                   "layout(set = 0, binding = "
                       + std::to_string(vulkanComputeTextureBinding(0))
                       + ") uniform sampler2D texture0;"));
    check(contains(glsl,
                   "layout(set = 0, binding = "
                       + std::to_string(vulkanComputeTextureBinding(1))
                       + ") uniform writeonly image2D texture1;"));
    check(contains(glsl,
                   "imageStore(texture1, ivec2(gid.x, gid.y), texture(texture0, "
                   "vec2(float(gid.x), float(gid.y))));"));
    check(!contains(glsl, "uint2(gid.x, gid.y)"));

    expectGlslCompiles(builder.graph());
};

// Two textures sampled two different ways get two SamplerStates, at the
// registers their configurations name - and a third texture sampled like the
// first gets no sampler of its own.
//
// This is the other half of codegenCubeTextureEmits, which checks that sharing
// happens; this checks that it is sharing rather than collapsing. Between them
// they pin the rule: one sampler per *configuration used*, never per texture
// and never one for the whole shader.
auto tCodegenSamplerPerConfiguration =
    test("GPU/codegenSamplerPerConfiguration") = []
{
    auto builder = ShaderBuilder {};

    const auto nearestClamp = TextureSampling {};
    const auto linearRepeat =
        TextureSampling {TextureFilter::Linear, TextureAddressMode::Repeat};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto crisp = builder.texture(nearestClamp);
    auto smooth = builder.texture(linearRepeat);
    auto alsoCrisp = builder.texture(nearestClamp);
    auto varyingUv = builder.varying(uv);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(sample(crisp, varyingUv) * sample(smooth, varyingUv)
                     * sample(alsoCrisp, varyingUv));

    auto hlsl = emitHlsl(builder.graph());

    const auto crispIndex = std::to_string(samplingIndex(nearestClamp));
    const auto smoothIndex = std::to_string(samplingIndex(linearRepeat));

    check(crispIndex != smoothIndex);

    check(contains(hlsl,
                   "SamplerState samplerConfig" + crispIndex + " : register(s"
                       + crispIndex + ");"));
    check(contains(hlsl,
                   "SamplerState samplerConfig" + smoothIndex + " : register(s"
                       + smoothIndex + ");"));

    check(contains(hlsl, "texture0.Sample(samplerConfig" + crispIndex + ","));
    check(contains(hlsl, "texture1.Sample(samplerConfig" + smoothIndex + ","));
    check(contains(hlsl, "texture2.Sample(samplerConfig" + crispIndex + ","));

    // Metal is unchanged by any of this: a sampler per texture, in the
    // signature, because MSL has no registers to share.
    auto metal = emitMetal(builder.graph());
    check(contains(metal, "sampler sampler0 [[sampler(0)]]"));
    check(contains(metal, "sampler sampler1 [[sampler(1)]]"));
    check(contains(metal, "sampler sampler2 [[sampler(2)]]"));

    expectGlslCompiles(builder.graph());
};

// A kernel whose only output is a texture is still a kernel: recording any
// store is what marks the graph as one, and a graph with no storage buffer at
// all emits none.
auto tCodegenComputeTextureOnly = test("GPU/codegenComputeTextureOnly") = []
{
    auto builder = ShaderBuilder {};

    auto target = builder.writableTexture();
    auto p = builder.threadPosition();
    auto shade = toFloat(p.x) * 0.25f;

    builder.write(target, p.x, p.y, float4(shade, shade, shade, 1.0f));

    auto shader = builder.build();
    check(shader.source.isCompute());

    for (const auto& dialect: everyDialect(builder.graph()))
    {
        check(!contains(dialect.source, "buffer0"));
        check(contains(dialect.source, "uniforms.width"));
        check(contains(dialect.source, "float t0 = (float(gid.x) * 0.25);"));
    }

    expectGlslCompiles(builder.graph());
};

// The 1D kernel keeps its scalar signature and its single count: the rank is a
// property of what the body asked for, not a new default.
auto tCodegenCompute1DUnchanged = test("GPU/codegenCompute1DKeepsScalarId") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.outputBuffer();
    auto gid = builder.threadId();

    builder.write(output, gid, toFloat(gid));

    auto shader = builder.build();
    check(shader.dispatchRank == DispatchRank::OneD);

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint gid [[thread_position_in_grid]]"));
    check(!contains(metal, "uint2 gid"));
    check(contains(metal, "if (gid >= uniforms.count)"));

    expectGlslCompiles(builder.graph());
};

// Vector reads and writes over a buffer of records: read4(i) is elements
// 4i..4i+3 and write(out, i, Float4) puts four back at the same place, so a
// kernel over a struct of four floats never spells the stride. The buffer is
// still a run of floats underneath - N scalar accesses, one index expression -
// which is what keeps its bytes bindable as a per-instance stream.
auto tCodegenComputeVectorElements = test("GPU/codegenComputeVectorElements") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto i = builder.threadId();

    auto record = input.read4(i);
    builder.write(output, i, record * 2.0f);

    // Nothing here is spelled per-language but the four-vector's own name, so
    // the whole body is one string checked three times.
    for (const auto& dialect: everyDialect(builder.graph()))
    {
        const auto& text = dialect.source;
        auto vec4 = std::string(dialect.spell(ValueType::Float4));

        // The base index is computed once for the whole kernel. The read and
        // the write build it through separate calls, but the product is the
        // same pure expression, so the graph hands both the one node.
        check(contains(text, "uint t0 = (gid * 4u);"));
        check(countOccurrences(text, "(gid * 4u)") == 1);

        // The three offsets off that base are shared the same way - each is
        // addressed by the read and by the write, so each is named once.
        check(contains(text, "uint t1 = (t0 + 1u);"));
        check(contains(text, "uint t2 = (t0 + 2u);"));
        check(contains(text, "uint t3 = (t0 + 3u);"));

        check(contains(text,
                       vec4 + " t4 = (" + vec4
                           + "(buffer0[t0], buffer0[t1], buffer0[t2], buffer0[t3]) "
                             "* 2.0);"));

        // One store per component, at the record's own offsets.
        check(contains(text, "buffer1[t0] = (t4).x;"));
        check(contains(text, "buffer1[t1] = (t4).y;"));
        check(contains(text, "buffer1[t2] = (t4).z;"));
        check(contains(text, "buffer1[t3] = (t4).w;"));
    }

    expectGlslCompiles(builder.graph());
};

// The narrower widths address their own records: read2 strides by two and
// read3 by three, so a buffer of pairs and one of triples each index in their
// own units.
auto tCodegenComputeVectorStrides = test("GPU/codegenComputeVectorStrides") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto i = builder.threadId();

    builder.write(output, i, input.read2(i));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint t0 = (gid * 2u);"));
    check(contains(metal, "float2 t2 = float2(buffer0[t0], buffer0[t1]);"));
    check(contains(metal, "buffer1[t1] = (t2).y;"));
    check(!contains(metal, "t0 + 2u"));

    auto triples = ShaderBuilder {};
    auto source = triples.inputBuffer();
    auto index = triples.threadId();
    triples.write(triples.outputBuffer(), index, source.read3(index));

    auto hlsl = emitHlsl(triples.graph());
    check(contains(hlsl, "uint t0 = (gid * 3u);"));
    check(contains(hlsl,
                   "float3 t3 = float3(buffer0[t0], buffer0[t1], buffer0[t2]);"));
    check(contains(hlsl, "buffer1[t2] = (t3).z;"));
    check(!contains(hlsl, "t0 + 3u"));

    expectGlslCompiles(builder.graph());
    expectGlslCompiles(triples.graph());
};

// A storage buffer read from a render stage: the same InputBuffer a kernel
// subscripts, declared by a graph with no stores at all, so the shader is a
// vertex/fragment pair rather than a kernel. What it buys is the indexed read a
// vertex attribute cannot do - the shader picks the element, instead of the
// input assembler handing it one.
auto tCodegenFragmentBufferRead = test("GPU/codegenFragmentBufferRead") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto palette = builder.inputBuffer();
    auto record = builder.uniform<UInt>();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(palette.read3(record), 1.0f));

    auto metal = emitMetal(builder.graph());

    // Declared on the stage that reads it and nowhere else: the vertex function
    // only builds a position, so a buffer parameter there would be dead weight
    // the caller still has to bind.
    check(contains(metal,
                   "device const float* buffer0 [[buffer("
                       + std::to_string(RenderPass::bufferBase) + ")]]"));
    check(countOccurrences(metal, "device const float* buffer0") == 1);
    check(metal.find("fragmentMain") < metal.find("device const float* buffer0"));

    // Read-only in a render stage, whatever a kernel would have got: there is
    // no writable buffer on this side.
    check(!contains(metal, "device float* buffer0"));

    // An HLSL global is visible to both functions, so it is declared once
    // outside either - above every texture register, which is what the render
    // root signature's buffer SRVs are declared at.
    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl,
                   "StructuredBuffer<float> buffer0 : register(t"
                       + std::to_string(RenderPass::bufferRegisterBase) + ");"));
    check(!contains(hlsl, "RWStructuredBuffer"));

    // A GLSL storage block takes no instance name, so its run of elements is a
    // global called buffer0 and every subscript below prints byte-identically
    // to the other two. The block itself still needs a name, and Buffer0 is the
    // only thing in the declaration with no counterpart elsewhere.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl,
                   "layout(std430, set = 0, binding = "
                       + std::to_string(vulkanBufferBinding(0))
                       + ") readonly buffer Buffer0\n{\n    float buffer0[];\n};"));
    check(!contains(glsl, "} buffer0;"));

    // The record index reaches the read in all three: read3 strides by three,
    // so an index the shader computed addresses its own record.
    for (const auto& dialect: everyDialect(builder.graph()))
    {
        auto vec3 = std::string(dialect.spell(ValueType::Float3));

        check(contains(dialect.source, "uint t0 = (uniforms.u0 * 3u);"));
        check(contains(
            dialect.source,
            vec3 + "(buffer0[t0], buffer0[(t0 + 1u)], buffer0[(t0 + 2u)])"));
    }

    expectGlslCompiles(builder.graph());
};

// The vertex stage reads one too, and gets its own parameter. Nothing about the
// binding is fragment-specific - which is what lets a vertex shader place a
// per-instance record it looked up rather than one it was handed.
auto tCodegenVertexBufferRead = test("GPU/codegenVertexBufferRead") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto offsets = builder.inputBuffer();
    auto record = builder.uniform<UInt>();

    builder.position(float4(position + offsets.read2(record), 0.0f, 1.0f));
    builder.fragment(float4(builder.constant(1.0f), 1.0f, 1.0f, 1.0f));

    auto metal = emitMetal(builder.graph());

    check(contains(metal,
                   "device const float* buffer0 [[buffer("
                       + std::to_string(RenderPass::bufferBase) + ")]]"));
    check(countOccurrences(metal, "device const float* buffer0") == 1);
    check(metal.find("device const float* buffer0") < metal.find("fragmentMain"));

    expectGlslCompiles(builder.graph());
};

// Both stages reading one buffer declare it once each, and the fragment stage's
// parameter list keeps textures and buffers in their own index spaces.
auto tCodegenBothStagesReadBuffer = test("GPU/codegenBothStagesReadBuffer") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto data = builder.inputBuffer();
    auto record = builder.uniform<UInt>();

    builder.position(float4(position * data[record], 0.0f, 1.0f));
    builder.fragment(float4(data[record + 1u], 0.0f, 0.0f, 1.0f));

    auto metal = emitMetal(builder.graph());
    check(countOccurrences(metal, "device const float* buffer0") == 2);

    auto hlsl = emitHlsl(builder.graph());
    check(countOccurrences(hlsl, "StructuredBuffer<float> buffer0") == 1);

    expectGlslCompiles(builder.graph());
};

// The integer vocabulary: the literal, the operators no float has, and the two
// explicit crossings between int and float arithmetic. All of it spells
// identically in MSL and HLSL, so both backends are checked against the same
// text. Pure string generation.
auto tCodegenIntegerOperators = test("GPU/codegenIntegerOperators") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto index = toInt(carried.x() * 4.0f) & 3;
    auto scrambled = ((index << 2) | (index >> 1)) ^ ~index;
    auto shade = toFloat(scrambled % 5) * 0.2f;

    builder.fragment(float4(shade, shade, shade, 1.0f));

    for (const auto& dialect: everyDialect(builder.graph()))
    {
        const auto& source = dialect.source;

        // An int literal carries no suffix - unlike a uint's - and the
        // truncating cast is a constructor-style one in all three languages.
        check(contains(
            source, "int t0 = (int(((" + dialect.varying(0) + ").x * 4.0)) & 3);"));

        // The two shifts, which are the only operators here that do not fit in
        // the char the graph carries an operator in.
        check(contains(source, "(t0 << 2)"));
        check(contains(source, "(t0 >> 1)"));

        check(contains(source, "(~(t0))"));
        check(contains(source, "float("));
    }

    // The modulus is the one operator here the three do not share. MSL and HLSL
    // truncate towards zero; GLSL leaves % undefined the moment either operand
    // is negative, which a coordinate left of the origin makes it. So that arm
    // writes the truncating remainder out of the division, which *is* defined
    // there - the same value, spelled at length.
    auto metal = emitMetal(builder.graph());
    check(contains(metal, "% 5)"));
    check(contains(emitHlsl(builder.graph()), "% 5)"));

    auto glsl = emitGlsl(builder.graph());
    check(!contains(glsl, "% 5)"));
    check(contains(glsl, " - (((((t0 << 2) | (t0 >> 1)) ^ (~(t0))) / 5) * 5))"));

    expectGlslCompiles(builder.graph());
};

// An Int crosses from the CPU, which is what separates it from a Bool and from
// the small matrices: MSL and HLSL both give a signed integer four bytes and
// pack it exactly where they pack a float, so the block needs no padding to
// reconcile them and a shader can be handed an index rather than a float to
// truncate. Pure string generation.
auto tCodegenIntegerUniform = test("GPU/codegenIntegerUniform") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto first = builder.uniform<Int>();
    auto scale = builder.uniform<Float>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto shade = toFloat(first + 1) * scale * carried.x();
    builder.fragment(float4(shade, shade, shade, 1.0f));

    for (const auto& dialect: everyDialect(builder.graph()))
    {
        check(contains(dialect.source, "int u0;"));
        check(contains(dialect.source, "float u1;"));
        check(contains(dialect.source, "float((uniforms.u0 + 1))"));

        // Two four-byte scalars in a row: all three rule sets agree on where the
        // second one lands, so nothing is padded between them.
        check(!contains(dialect.source, "pad"));
    }

    // And the CPU block is what the two of them add up to.
    auto types = Vector<ValueType> {};
    types.add(ValueType::Int);
    types.add(ValueType::Float);

    auto offsets = uniformOffsets(types);
    check(offsets[0] == 0);
    check(offsets[1] == 4);

    expectGlslCompiles(builder.graph());
};

// A constant array is declared once at the top of the stage that subscripts it,
// and nowhere else: not in the vertex function, which never reads it. Pure
// string generation.
auto tCodegenConstantArray = test("GPU/codegenConstantArray") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto palette = builder.array(float3(builder.constant(0.1f), 0.1f, 0.2f),
                                 float3(builder.constant(0.9f), 0.4f, 0.2f),
                                 float3(builder.constant(0.2f), 0.8f, 0.6f),
                                 float3(builder.constant(1.0f), 0.9f, 0.7f));

    auto index = toInt(carried.x() * 4.0f) & 3;
    auto picked = palette[index];

    builder.fragment(float4(picked * 0.5f + picked * 0.5f + palette[0], 1.0f));

    for (const auto& dialect: everyDialect(builder.graph()))
    {
        const auto& source = dialect.source;

        // GLSL is the one dialect that cannot say const here: an element may be
        // computed from a uniform, and a const there has to be a constant
        // expression. Nothing assigns to the array either way.
        auto vec3 = std::string(dialect.spell(ValueType::Float3));
        auto qualifier = std::string(dialect.glsl ? "" : "const ");

        auto declaration = qualifier + vec3 + " a0[4] = {" + vec3
                           + "(0.1, 0.1, 0.2), " + vec3 + "(0.9, 0.4, 0.2), " + vec3
                           + "(0.2, 0.8, 0.6), " + vec3 + "(1.0, 0.9, 0.7)};";

        auto read = vec3 + " t0 = (a0[(int(((" + dialect.varying(0)
                    + ").x * 4.0)) & 3)] * 0.5);";

        check(countOccurrences(source, declaration) == 1);

        // The subscript and the scale above it are one pure expression written
        // twice, so they collapse to a single name: the array is indexed once
        // and the multiply runs once, whatever the shader spelled.
        check(countOccurrences(source, read) == 1);
        check(countOccurrences(source, "t0") == 3);

        // The literal subscript, and the declaration before either read.
        check(contains(source, "a0[0]"));
        check(source.find(declaration) < source.find(read));

        // The array lives in the fragment stage, which is the only one that
        // reads it.
        check(source.find(dialect.fragmentStage()) < source.find(declaration));
    }

    expectGlslCompiles(builder.graph());
};

// The integer vectors: built out of a coordinate, taken apart by component, put
// back together, and carrying the operators only an integer has - all of it
// componentwise. Both languages spell the type and every operation on it the
// same way, so both backends are checked against the same text. Pure string
// generation.
auto tCodegenIntegerVectors = test("GPU/codegenIntegerVectors") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    // The whole vector crosses in one cast, not a component at a time.
    auto cell = toInt(carried * 16.0f);

    // Componentwise against another vector, against a broadcast literal, and
    // the shift no float has.
    auto wrapped = (cell & 7) + int2(cell.y(), cell.x());
    auto shifted = wrapped << 1;

    auto shade = toFloat(shifted.x() + shifted.y()) * 0.01f;
    auto tint = toFloat(-cell) * 0.001f;

    builder.fragment(float4(shade + tint.x(), shade, shade, 1.0f));

    for (const auto& dialect: everyDialect(builder.graph()))
    {
        const auto& source = dialect.source;
        auto ivec2 = std::string(dialect.spell(ValueType::Int2));
        auto vec2 = std::string(dialect.spell(ValueType::Float2));

        // The whole vector crosses in one cast rather than a component at a
        // time, which is what keeps the coordinate behind it recorded once.
        check(contains(source,
                       ivec2 + " t0 = " + ivec2 + "((" + dialect.varying(0)
                           + " * 16.0));"));

        // The mask broadcasts the literal, the constructor takes two integer
        // components, and the shift is the operator no float has.
        check(contains(source,
                       ivec2 + " t1 = (((t0 & 7) + " + ivec2
                           + "((t0).y, (t0).x)) << 1);"));

        // A component of an integer vector is an integer, so the crossing back
        // into float arithmetic is still spelled out.
        check(contains(source, "float(((t1).x + (t1).y))"));

        // And the whole vector crosses back in one piece too.
        check(contains(source, vec2 + "((-(t0)))"));
    }

    expectGlslCompiles(builder.graph());
};

// The componentwise comparison and what collapses it. This is the pair that
// makes the boolean vector worth having: `<` on two vectors is a mask in both
// languages, and all()/any() is the only thing that turns one into a condition
// a branch or a select can take. Pure string generation.
auto tCodegenVectorComparison = test("GPU/codegenVectorComparison") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto limit = builder.uniform<Float2>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto inside = carried < limit;
    auto outside = !inside;

    auto lit = select(all(inside), 1.0f, 0.25f);
    auto edge = select(any(outside), 0.5f, 0.0f);

    builder.fragment(float4(lit, edge, lit, 1.0f));

    // MSL and HLSL give the operator itself to a vector pair; GLSL reserves it
    // for scalars and spells the componentwise form as a function, negation
    // included. This is the pair of dialects where the EDSL's one node has two
    // genuinely different shapes rather than two names.
    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        // The comparison is the operator itself, and its result is a mask of
        // the operands' width rather than a scalar.
        check(contains(source, "bool2 t0 = (input.v0 < uniforms.u0);"));

        // The negation is the operator too.
        check(contains(source, "any((!(t0)))"));

        // And the mask reaches a select only through a collapse - the whole
        // reason for having the type at all.
        check(contains(source, "all(t0) ?"));
    }

    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "bvec2 t0 = lessThan(vary0, uniforms.u0);"));
    check(contains(glsl, "any(not(t0))"));
    check(contains(glsl, "all(t0) ?"));

    // Neither the operator nor the scalar negation survives into the GLSL:
    // `vary0 < uniforms.u0` there is a compile error, not a mask.
    check(!contains(glsl, "(vary0 < uniforms.u0)"));
    check(!contains(glsl, "(!(t0))"));

    expectGlslCompiles(builder.graph());
};

// An Int2 crosses from the CPU where a Bool2 does not, and packs exactly where
// a Float2 does - so the block needs no padding to reconcile the two backends,
// and a shader can be handed a grid cell rather than a pair of floats to
// truncate. Pure string generation.
auto tCodegenIntegerVectorUniform = test("GPU/codegenIntegerVectorUniform") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto origin = builder.uniform<Int2>();
    auto scale = builder.uniform<Float>();
    auto carried = builder.varying(position);

    builder.position(float4(position, 0.0f, 1.0f));

    auto cell = toInt(carried * 16.0f) - origin;
    auto shade = toFloat(cell.x() + cell.y()) * scale;

    builder.fragment(float4(shade, shade, shade, 1.0f));

    for (const auto& dialect: everyDialect(builder.graph()))
    {
        check(contains(dialect.source,
                       std::string(dialect.spell(ValueType::Int2)) + " u0;"));
        check(contains(dialect.source, "float u1;"));
        check(contains(dialect.source, "- uniforms.u0)"));

        // An eight-byte value followed by a four-byte one: all three rule sets
        // agree on where the second lands, so nothing is padded between them.
        check(!contains(dialect.source, "pad"));
    }

    auto types = Vector<ValueType> {};
    types.add(ValueType::Int2);
    types.add(ValueType::Float);

    auto offsets = uniformOffsets(types);
    check(offsets[0] == 0);
    check(offsets[1] == 8);

    expectGlslCompiles(builder.graph());
};

// A shared-memory reduction kernel end to end in text: the threadgroup tile,
// the local and group ids in the entry signature, the barrier - and the guard
// that is not there. A kernel that barriers gets no early return, because a
// barrier below a return some threads took is undefined on both backends;
// what bounds its loads instead is gridCount(), the same value the guard
// would have read.
auto tCodegenComputeSharedReduction = test("GPU/codegenComputeSharedReduction") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto gid = builder.threadId();
    auto lid = builder.localId();
    auto group = builder.groupId();
    auto tile = builder.shared<Float>(64);

    auto value = builder.var(0.0f);
    builder.ifThen(gid < builder.gridCount(), [&] { value = input[gid]; });
    builder.write(tile, lid, value.get());
    builder.barrier();

    builder.ifThen(lid < 32u,
                   [&] { builder.write(tile, lid, tile[lid] + tile[lid + 32u]); });
    builder.barrier();

    builder.ifThen(lid == 0u, [&] { builder.write(output, group, tile[0u]); });

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint lid [[thread_position_in_threadgroup]]"));
    check(contains(metal, "uint tgid [[threadgroup_position_in_grid]]"));
    check(contains(metal, "threadgroup float s0[64];"));
    check(contains(metal, "threadgroup_barrier(mem_flags::mem_threadgroup);"));
    check(!contains(metal, "return;"));
    check(contains(metal, "if ((gid < uniforms.count))"));
    check(contains(metal, "s0[lid] = v0;"));
    check(contains(metal, "s0[lid] = (s0[lid] + s0[(lid + 32u)]);"));
    check(contains(metal, "buffer1[tgid] = s0[0u];"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "groupshared float s0[64];"));
    check(contains(hlsl, "uint3 localThread : SV_GroupThreadID"));
    check(contains(hlsl, "uint3 groupIndex : SV_GroupID"));
    check(contains(hlsl, "uint lid = localThread.x;"));
    check(contains(hlsl, "uint tgid = groupIndex.x;"));
    check(contains(hlsl, "GroupMemoryBarrierWithGroupSync();"));
    check(!contains(hlsl, "return;"));
    check(contains(hlsl, "s0[lid] = (s0[lid] + s0[(lid + 32u)]);"));
    check(contains(hlsl, "buffer1[tgid] = s0[0u];"));

    // A GLSL threadgroup array is a global as on HLSL, and the barrier is the
    // one statement here that is two calls rather than one: the memory barrier
    // publishes what was written to the tile, the execution barrier is where the
    // group meets.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "shared float s0[64];"));
    check(contains(glsl, "    uint lid = gl_LocalInvocationID.x;"));
    check(contains(glsl, "    uint tgid = gl_WorkGroupID.x;"));
    check(countOccurrences(glsl, "    memoryBarrierShared();\n    barrier();\n")
          == 2);
    check(!contains(glsl, "return;"));
    check(contains(glsl, "s0[lid] = (s0[lid] + s0[(lid + 32u)]);"));
    check(contains(glsl, "buffer1[tgid] = s0[0u];"));

    // The declaration is a global, not a body-scope local: it sits above
    // main(), which is where every thread in the group can see it.
    check(glsl.find("shared float s0[64];") < glsl.find("void main()"));

    expectGlslCompiles(builder.graph());
};

// A name computed from shared memory does not survive a barrier: what the
// tile held before other threads' stores were published is not what it holds
// after, so the emitter re-reads rather than reusing the local - the same
// rule an assignment imposes on the names that read its variable.
auto tCodegenComputeSharedNamesRetire =
    test("GPU/codegenComputeSharedNamesRetire") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.outputBuffer();
    auto gid = builder.threadId();
    auto lid = builder.localId();
    auto tile = builder.shared<Float>(64);

    builder.write(tile, lid, toFloat(gid));
    builder.barrier();

    // Used twice, so it takes a name.
    auto sum = tile[lid] + 1.0f;
    builder.write(output, gid, sum * sum);

    builder.barrier();

    // The same handle used twice again: the pre-barrier name is gone, so the
    // element is read - and named - afresh.
    builder.write(output, gid + 1u, sum * sum);

    auto metal = emitMetal(builder.graph());
    check(countOccurrences(metal, "s0[lid] + 1.0") == 2);
    check(contains(metal, "float t0 = (s0[lid] + 1.0);"));
    check(contains(metal, "float t1 = (s0[lid] + 1.0);"));
    check(contains(metal, "buffer0[gid] = (t0 * t0);"));
    check(contains(metal, "buffer0[(gid + 1u)] = (t1 * t1);"));

    expectGlslCompiles(builder.graph());
};

// The 2D siblings and a wide element type: localPosition()/groupPosition()
// ride the pair scaffolding exactly as threadPosition() does, and a shared
// array of float4 declares its element type verbatim - it never crosses the
// CPU boundary, so there is no scalar-layout contract to decompose it into.
auto tCodegenComputeShared2DFloat4 = test("GPU/codegenComputeShared2DFloat4") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto position = builder.threadPosition();
    auto local = builder.localPosition();
    auto group = builder.groupPosition();
    auto tile = builder.shared<Float4>(64);

    auto flatLocal = local.y * 8u + local.x;
    builder.write(
        tile, flatLocal, input.read4(position.y * builder.gridWidth() + position.x));
    builder.barrier();

    auto picked = tile[group.x % 8u + group.y];
    builder.write(output, position.y * builder.gridWidth() + position.x, picked);

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "uint2 lid [[thread_position_in_threadgroup]]"));
    check(contains(metal, "uint2 tgid [[threadgroup_position_in_grid]]"));
    check(contains(metal, "threadgroup float4 s0[64];"));
    check(contains(metal, "s0[((lid.y * 8u) + lid.x)] = "));
    check(contains(metal, "uniforms.width"));
    check(!contains(metal, "return;"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "groupshared float4 s0[64];"));
    check(contains(hlsl, "uint2 lid = localThread.xy;"));
    check(contains(hlsl, "uint2 tgid = groupIndex.xy;"));

    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "shared vec4 s0[64];"));
    check(contains(glsl, "    uvec2 lid = gl_LocalInvocationID.xy;"));
    check(contains(glsl, "    uvec2 tgid = gl_WorkGroupID.xy;"));
    check(contains(glsl, "s0[((lid.y * 8u) + lid.x)] = "));
    check(!contains(glsl, "return;"));

    expectGlslCompiles(builder.graph());
};

// The atomic pair in GLSL, which is the dialect the graph's statement shape was
// already right for: atomicAdd returns the old value the way MSL's fetch-add
// does, so unlike HLSL's InterlockedAdd it needs no name declared ahead of the
// call. A load is a plain subscript - a uint in a storage block is already what
// an atomic operates on, as an HLSL UAV element is - so only the element type
// of the block says the buffer is atomic at all.
auto tCodegenGlslAtomics = test("GPU/codegenGlslAtomics") = []
{
    auto builder = ShaderBuilder {};

    auto counter = builder.atomicBuffer();
    auto output = builder.outputBuffer();
    auto id = builder.threadId();
    auto ticket = builder.atomicAdd(counter, 0u, 1u);

    builder.write(output, id, toFloat(ticket) + toFloat(counter.load(0u)));

    auto glsl = emitGlsl(builder.graph());

    check(contains(glsl,
                   "layout(std430, set = 0, binding = "
                       + std::to_string(vulkanComputeBufferBinding(0))
                       + ") buffer Buffer0\n{\n    uint buffer0[];\n};"));
    check(contains(glsl, "    uint v0 = atomicAdd(buffer0[0u], 1u);\n"));
    check(contains(glsl, "float(buffer0[0u])"));

    // One statement, not two: the name and the call are one line, which is the
    // shape the other two dialects only half share.
    check(!contains(glsl, "uint v0;\n"));

    // The block is read-write, so it carries no readonly - and the buffer that
    // is not atomic keeps its float elements, so declaring one atomic buffer
    // does not retype the rest.
    check(!contains(glsl, "readonly buffer Buffer0"));
    check(contains(glsl, "buffer Buffer1\n{\n    float buffer1[];\n};"));

    expectGlslCompiles(builder.graph());
};

// The intrinsics that arrived after the GLSL dialect did, three the language
// has and three it does not. sinh, cosh and tanh go out as themselves; erf and
// erfc take the polynomial helper the other two dialects also carry, spelled
// in vec2/3/4; and log10, a builtin in MSL and HLSL, is the one call GLSL has
// no name for, so on this dialect alone it is renamed to a helper the table
// defines - and on the other two it stays the builtin with no helper at all.
auto tCodegenGlslIntrinsicHelpers = test("GPU/codegenGlslIntrinsicHelpers") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto i = builder.threadId();
    auto x = input[i];
    auto v = input.read4(i);

    builder.write(output,
                  i,
                  erf(x) + erfc(x) + log10(x) + sinh(x) + cosh(x) + tanh(x)
                      + erf(v).x() + erfc(v).y() + log10(v).z());

    const auto& graph = builder.graph();
    auto glsl = emitGlsl(graph);

    check(contains(glsl, "float eacpErf(float x)"));
    check(contains(glsl, "vec4 eacpErf(vec4 x)"));
    check(contains(glsl, "float eacpErfc(float x)"));
    check(contains(glsl, "vec4 eacpErfc(vec4 x)"));
    check(contains(glsl, "float eacpLog10(float x)"));
    check(contains(glsl, "vec4 eacpLog10(vec4 x)"));
    check(contains(glsl, "eacpLog10("));
    check(contains(glsl, "sinh("));
    check(contains(glsl, "cosh("));
    check(contains(glsl, "tanh("));

    // Nothing of the other two dialects reaches the GLSL: no float2/3/4 in a
    // helper body, and no call to the builtin GLSL lacks. The helper's own
    // name has a capital L, so the second check is only about the builtin.
    check(!contains(glsl, "float2"));
    check(!contains(glsl, "float4"));
    check(!contains(glsl, "log10("));

    check(!contains(emitMetal(graph), "eacpLog10"));
    check(!contains(emitHlsl(graph), "eacpLog10"));
    check(contains(emitMetal(graph), "log10("));
    check(contains(emitHlsl(graph), "log10("));

    expectGlslCompiles(graph);
};

// The fp16 storage helpers in GLSL: unpackHalf2x16 and packHalf2x16 do in one
// builtin what MSL spells as a bitcast through half2 and HLSL as f16tof32 and
// f32tof16, and as_type<float> is uintBitsToFloat.
auto tCodegenGlslHalfHelpers = test("GPU/codegenGlslHalfHelpers") = []
{
    auto builder = ShaderBuilder {};

    auto weights = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto i = builder.threadId();

    builder.write(output, i, weights.readHalf(i));
    builder.write(output, i + 1u, asFloat(packHalf2(weights.readHalf2(i))));
    builder.writeHalf2(output, i + 2u, weights.readHalf2(i + 1u));

    const auto& graph = builder.graph();
    auto glsl = emitGlsl(graph);

    check(contains(glsl, "float eacpReadHalf(uint bits, uint parity)"));
    check(contains(glsl, "unpackHalf2x16(bits >> (16u * parity)).x"));
    check(contains(glsl, "vec2 eacpUnpackHalf2(uint bits)"));
    check(contains(glsl, "uint eacpPackHalf2(vec2 values)"));
    check(contains(glsl, "packHalf2x16(values)"));
    check(contains(glsl, "uintBitsToFloat("));
    check(contains(glsl, "floatBitsToUint("));

    check(!contains(glsl, "as_type"));
    check(!contains(glsl, "f16tof32"));
    check(!contains(glsl, "float2"));

    expectGlslCompiles(graph);
};

// A depth slot is where GLSL breaks the rule the other two keep: a sampler2D
// over a depth image hands back four channels whatever the image holds, so the
// call has to take the first one to be the single float the EDSL's handle says
// it is. On MSL and HLSL the declared type does that and the call is untouched.
auto tCodegenGlslDepthSample = test("GPU/codegenGlslDepthSample") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto color = builder.texture();
    auto depth = builder.depthTexture();
    auto carried = builder.varying(uv);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(sample(color, carried).xyz(), sample(depth, carried)));

    auto metal = emitMetal(builder.graph());
    check(contains(metal, "depth2d<float> texture1 [[texture(1)]]"));
    check(contains(metal, "texture1.sample(sampler1, input.v0)"));
    check(!contains(metal, "texture1.sample(sampler1, input.v0).r"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "Texture2D<float> texture1 : register(t1);"));
    check(contains(hlsl, "texture1.Sample(samplerConfig0, input.v0)"));
    check(!contains(hlsl, "texture1.Sample(samplerConfig0, input.v0).r"));

    // The GLSL declaration is the ordinary sampler2D - there is no depth type to
    // declare - and the .r is on the sample of the depth slot alone.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl,
                   "layout(set = 0, binding = "
                       + std::to_string(vulkanTextureBinding(1))
                       + ") uniform sampler2D texture1;"));
    check(contains(glsl, "texture(texture1, vary0).r"));
    check(contains(glsl, "texture(texture0, vary0)"));
    check(!contains(glsl, "texture(texture0, vary0).r"));
    check(countOccurrences(glsl, ").r") == 1);

    expectGlslCompiles(builder.graph());
};

// Clip space is left exactly as the graph computed it. Vulkan's NDC has y down
// where Metal's and D3D's have it up, and the fix for that is a negative
// viewport height in RenderPass - not a negation here, which would reverse the
// winding along with the picture and take CullModeTests with it. Depth is
// already [0, 1] in all three, so the projection is untouched too.
auto tCodegenGlslKeepsClipPosition = test("GPU/codegenGlslKeepsClipPosition") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto scale = builder.uniform<Float2>();

    builder.position(float4(position * scale, 0.25f, 1.0f));
    builder.fragment(float4(builder.constant(1.0f), 1.0f, 1.0f, 1.0f));

    auto glsl = emitGlsl(builder.graph());

    // The one write to the builtin, and it is the expression as written.
    check(
        contains(glsl, "    gl_Position = vec4((attr0 * uniforms.u0), 0.25, 1.0);"));
    check(countOccurrences(glsl, "gl_Position") == 1);
    check(!contains(glsl, "gl_Position.y"));

    // And the same expression reaches the other two, so no dialect is carrying a
    // coordinate convention the others are not.
    check(contains(emitMetal(builder.graph()),
                   "    output.position = float4((input.a0 * uniforms.u0), 0.25, "
                   "1.0);"));

    expectGlslCompiles(builder.graph());
};

// `input` and `output` are reserved words in GLSL, which the struct-passing
// dialects use as the name of a stage's I/O. Nothing in the GLSL arm may spell
// either, whatever the shader declared - so this runs a graph that has one of
// everything and looks for both words as whole identifiers.
auto tCodegenGlslAvoidsReservedWords =
    test("GPU/codegenGlslAvoidsReservedWords") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto uv = builder.vertexInput<Float2>();
    auto image = builder.texture();
    auto data = builder.inputBuffer();
    auto record = builder.uniform<UInt>();
    auto carried = builder.varying(uv);

    builder.position(float4(position, 0.0f, 1.0f));

    auto shade = builder.var(0.0f);
    builder.ifThen(carried.x() > 0.5f, [&] { shade = data[record]; });

    builder.fragment(sample(image, carried) * shade.get());

    auto glsl = emitGlsl(builder.graph());
    check(!contains(glsl, "input"));
    check(!contains(glsl, "output"));

    // The two renames that make that true: an attribute is attrN and a varying
    // varyN, neither of which collides with the constant arrays (aN) or the
    // mutable locals (vN) the same shader can hold.
    check(contains(glsl, "in vec2 attr0;"));
    check(contains(glsl, "in vec2 vary0;"));
    check(contains(glsl, "float v0 = 0.0;"));

    // And the kernel half, where the storage blocks and the ids are what would
    // otherwise have wanted the words.
    auto kernel = ShaderBuilder {};
    auto kernelInput = kernel.inputBuffer();
    auto kernelOutput = kernel.outputBuffer();
    auto gid = kernel.threadId();
    kernel.write(kernelOutput, gid, kernelInput[gid]);

    check(!contains(emitGlsl(kernel.graph()), "input"));
    check(!contains(emitGlsl(kernel.graph()), "output"));

    expectGlslCompiles(builder.graph());
    expectGlslCompiles(kernel.graph());
};

// A scalar written beside a vector, in every argument position the EDSL allows
// one. MSL converts it and HLSL promotes it, so both take the call as written;
// GLSL overloads each genType builtin per width and accepts a scalar only in a
// few trailing positions, so every scalar argument of a mixed call is broadcast
// there through the call's own vector constructor.
//
// The case that found this is the UI shape shader's length(max(0.f, q)): a
// scalar first, which no GLSL overload of max has, and which the emitter had
// been printing verbatim.
auto tCodegenGlslScalarBesideVector = test("GPU/codegenGlslScalarBesideVector") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto width = builder.uniform<Float>();

    builder.position(float4(position, 0.0f, 1.0f));

    auto q = builder.varying(position);
    auto scalar = q.x() + q.y();

    // The literal first, the literal second, a computed scalar in either of
    // clamp's bounds, mix's amount, step's edge, both of smoothstep's, and pow's
    // exponent - the whole of what ShapedBeside admits.
    auto outside = length(max(0.0f, q));
    auto floored = min(q, 0.0f);
    auto held = clamp(q, 0.0f, width);
    auto blended = mix(q, floored, scalar);
    auto gated = step(0.0f, q);
    auto ramp = smoothstep(0.0f, width, q);
    auto curved = pow(q, 2.0f);
    auto angle = atan2(q, 1.0f);

    builder.fragment(
        float4(held + blended + gated, ramp.x() + curved.y() + angle.x(), outside));

    // Unchanged in the two dialects that need no broadcast: the scalar prints
    // exactly where it was written.
    auto metal = emitMetal(builder.graph());
    check(contains(metal, "max(0.0, "));
    check(contains(metal, ", 0.0)"));
    check(contains(metal, "clamp("));
    check(!contains(metal, "float2(0.0)"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "max(0.0, "));
    check(contains(hlsl, ", 0.0)"));
    check(!contains(hlsl, "float2(0.0)"));

    // And broadcast in the one that does, in every position.
    auto glsl = emitGlsl(builder.graph());
    check(contains(glsl, "max(vec2(0.0), "));
    check(contains(glsl, "min(vary0, vec2(0.0))"));
    check(contains(glsl, "vec2(0.0), vec2(uniforms.u0))"));
    check(contains(glsl, "step(vec2(0.0), vary0)"));
    check(contains(glsl, "pow(vary0, vec2(2.0))"));
    check(contains(glsl, "atan(vary0, vec2(1.0))"));
    check(!contains(glsl, "max(0.0, "));
    check(!contains(glsl, ", 0.0)"));

    // A call whose arguments already agree keeps the scalar form: nothing is
    // broadcast to a width of one.
    auto scalarOnly = ShaderBuilder {};
    auto scalarPosition = scalarOnly.vertexInput<Float2>();
    auto carried = scalarOnly.varying(scalarPosition);

    scalarOnly.position(float4(scalarPosition, 0.0f, 1.0f));
    scalarOnly.fragment(
        float4(smoothstep(0.0f, 1.0f, carried.x()), 0.0f, 0.0f, 1.0f));

    check(contains(emitGlsl(scalarOnly.graph()), "smoothstep(0.0, 1.0, "));

    expectGlslCompiles(builder.graph());
    expectGlslCompiles(scalarOnly.graph());
};
