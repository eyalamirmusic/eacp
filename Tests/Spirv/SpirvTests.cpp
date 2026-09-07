#include <NanoTest/NanoTest.h>
#include <eacp/GPU/Spirv/SpirvCompiler.h>

#include <chrono>
#include <iostream>
#include <thread>

using namespace nano;
using namespace eacp::GPU::Spirv;

namespace
{
constexpr auto spirvMagic = uint32_t {0x07230203};

// The binding map the EDSL emits: std140 at set 0 binding 0, samplers from 8.
std::string texturedQuadSource()
{
    return R"(#version 450

layout(std140, set = 0, binding = 0) uniform Uniforms
{
    mat4 modelViewProjection;
    vec4 tint;
} uniforms;

layout(set = 0, binding = 8) uniform sampler2D albedo;

vec4 tinted(vec4 colour)
{
    return colour * uniforms.tint;
}

#ifdef EACP_VERTEX
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 texCoord;
layout(location = 0) out vec2 varyingTexCoord;

void main()
{
    varyingTexCoord = texCoord;
    gl_Position = uniforms.modelViewProjection * vec4(position, 1.0);
}
#endif

#ifdef EACP_FRAGMENT
layout(location = 0) in vec2 varyingTexCoord;
layout(location = 0) out vec4 fragmentColour;

void main()
{
    fragmentColour = tinted(texture(albedo, varyingTexCoord));
}
#endif
)";
}

std::string prefixSumSource()
{
    return R"(#version 450

layout(local_size_x = 64) in;

layout(std140, set = 0, binding = 0) uniform Uniforms
{
    uint elementCount;
} uniforms;

layout(std430, set = 0, binding = 24) buffer Values
{
    float values[];
} valuesBuffer;

void main()
{
    uint index = gl_GlobalInvocationID.x;

    if (index < uniforms.elementCount)
        valuesBuffer.values[index] = valuesBuffer.values[index] * 2.0;
}
)";
}

// The undeclared identifier is on line 8, which is what the log has to name.
std::string undeclaredIdentifierSource()
{
    return R"(#version 450

#ifdef EACP_FRAGMENT
layout(location = 0) out vec4 fragmentColour;

void main()
{
    fragmentColour = vec4(missingSymbol, 0.0, 0.0, 1.0);
}
#endif
)";
}

std::string fragmentOnlySource()
{
    return R"(#version 450

#ifdef EACP_FRAGMENT
layout(location = 0) out vec4 fragmentColour;

void main()
{
    fragmentColour = vec4(1.0);
}
#endif
)";
}

bool contains(const std::string& text, std::string_view fragment)
{
    return text.find(fragment) != std::string::npos;
}

void checkIsSpirvModule(const CompileResult& result)
{
    check(result.succeeded());
    check(result.log.empty());
    check(!result.words.empty());
    check(result.words[0] == spirvMagic);
}

double millisecondsToCompile(Stage stage, const std::string& source)
{
    const auto start = std::chrono::steady_clock::now();
    const auto result = compileGlsl(stage, source);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    check(result.succeeded());

    return std::chrono::duration<double, std::milli> {elapsed}.count();
}
} // namespace

auto tOneSourceCompilesBothStages = test("Spirv/oneSourceCompilesBothStages") = []
{
    const auto source = texturedQuadSource();

    checkIsSpirvModule(compileGlsl(Stage::Vertex, source));
    checkIsSpirvModule(compileGlsl(Stage::Fragment, source));
};

auto tStageMacrosSelectDifferentModules =
    test("Spirv/stageMacrosSelectDifferentModules") = []
{
    const auto source = texturedQuadSource();
    const auto vertex = compileGlsl(Stage::Vertex, source);
    const auto fragment = compileGlsl(Stage::Fragment, source);

    check(vertex.succeeded());
    check(fragment.succeeded());
    check(vertex.words.size() != fragment.words.size());
};

auto tComputeSourceCompiles = test("Spirv/computeSourceCompiles") = []
{ checkIsSpirvModule(compileGlsl(Stage::Compute, prefixSumSource())); };

auto tUniformBlockAndSamplerCompile =
    test("Spirv/uniformBlockAndSamplerCompile") = []
{ checkIsSpirvModule(compileGlsl(Stage::Fragment, texturedQuadSource())); };

auto tErrorReportsLineAndIdentifier =
    test("Spirv/errorReportsLineAndIdentifier") = []
{
    const auto result = compileGlsl(Stage::Fragment, undeclaredIdentifierSource());

    check(!result.succeeded());
    check(result.words.empty());
    check(contains(result.log, "missingSymbol"));
    check(contains(result.log, ":8:"));
};

auto tMissingStageBlockFails = test("Spirv/missingStageBlockFails") = []
{
    const auto result = compileGlsl(Stage::Vertex, fragmentOnlySource());

    check(!result.succeeded());
    check(result.words.empty());
    check(!result.log.empty());

    // The same source is a valid fragment shader, so only the macro differs.
    check(compileGlsl(Stage::Fragment, fragmentOnlySource()).succeeded());
};

auto tWarmUpIsIdempotent = test("Spirv/warmUpIsIdempotent") = []
{
    warmUp();
    warmUp();

    checkIsSpirvModule(compileGlsl(Stage::Vertex, texturedQuadSource()));
};

// Both threads race into the built-in symbol table construction, which is the
// one piece of glslang state a TShader does not own privately.
auto tConcurrentCompilesWork = test("Spirv/concurrentCompilesWork") = []
{
    auto vertex = CompileResult {};
    auto fragment = CompileResult {};

    auto vertexWorker = std::thread(
        [&vertex] { vertex = compileGlsl(Stage::Vertex, texturedQuadSource()); });

    auto fragmentWorker = std::thread(
        [&fragment]
        { fragment = compileGlsl(Stage::Fragment, texturedQuadSource()); });

    vertexWorker.join();
    fragmentWorker.join();

    checkIsSpirvModule(vertex);
    checkIsSpirvModule(fragment);
};

auto tCompileTimings = test("Spirv/compileTimings") = []
{
    const auto source = texturedQuadSource();
    const auto first = millisecondsToCompile(Stage::Vertex, source);

    auto steady = 0.0;
    constexpr auto steadyRuns = 20;

    for (auto run = 0; run < steadyRuns; ++run)
        steady += millisecondsToCompile(Stage::Fragment, source);

    std::cout << "Spirv: first compile in this process " << first
              << " ms, steady state " << steady / steadyRuns << " ms per stage\n";
};
