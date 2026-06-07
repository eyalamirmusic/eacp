#include <eacp/GPU/GPU.h>

#include <NanoTest/NanoTest.h>

#include <string>

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

// Rotates the vertex position by a per-frame uniform angle, computed in-shader
// with sin/cos. Mirrors the RotatingTriangle demo.
GeneratedShader makeRotatingShader()
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
    auto rotated = float2(px * c - py * s, px * s + py * c);

    builder.position(float4(rotated, 0.0f, 1.0f));
    builder.fragment(float4(varyingColor, 1.0f));

    return builder.build();
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
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
};

// Feeds the generated source through the real platform shader compiler and
// builds a pipeline from the generated layout. Self-skips on hosts without a GPU
// device (matches GPUSmokeTests).
auto tCodegenCompiles = test("GPU/codegenCompiles") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto shader = makeTriangleShader();

    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout;

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
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
    check(contains(metal, "constant Uniforms& uniforms [[buffer(1)]]"));
    check(contains(metal, "cos(uniforms.u0)"));
    check(contains(metal, "sin(uniforms.u0)"));

    auto hlsl = emitHlsl(builder.graph());
    check(contains(hlsl, "cbuffer UniformsCB : register(b0)"));
    check(contains(hlsl, "cos(uniforms.u0)"));
    check(contains(hlsl, "sin(uniforms.u0)"));

    // The plain triangle declares no uniforms, so no block is emitted.
    auto plain = makeTriangleShader();
    check(!contains(plain.source.source, "Uniforms"));
};

// Compiles the rotating shader (with its uniform block) through the real
// platform shader compiler. Self-skips without a GPU device.
auto tCodegenUniformCompiles = test("GPU/codegenUniformCompiles") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto shader = makeRotatingShader();

    auto library = device.makeShaderLibrary(shader.source);
    check(library.isValid());

    auto descriptor = RenderPipelineDescriptor {};
    descriptor.library = &library;
    descriptor.vertexLayout = shader.vertexLayout;

    auto pipeline = device.makeRenderPipeline(descriptor);
    check(pipeline.isValid());
};
