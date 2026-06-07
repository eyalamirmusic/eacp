#include <eacp/Graphics/Graphics.h>
#include <eacp/GPU/GPU.h>

using namespace eacp;
using namespace GPU;

namespace
{
struct Vertex
{
    float position[2];
    float color[3];
};

const Vertex triangleVertices[] = {
    {{0.0f, 0.8f}, {1.0f, 0.2f, 0.2f}},
    {{-0.8f, -0.8f}, {0.2f, 1.0f, 0.2f}},
    {{0.8f, -0.8f}, {0.2f, 0.2f, 1.0f}},
};

// The same triangle as the Triangle demo, but the shader is described in C++ and
// the Metal/HLSL source plus the vertex layout are generated. No .metal/.hlsl
// files, no ResEmbed.
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
} // namespace

struct TriangleView final : GPUView
{
    TriangleView()
        : shader(makeTriangleShader())
        , vertexBuffer(Device::shared().makeBuffer(triangleVertices))
        , library(Device::shared().makeShaderLibrary(shader.source))
        , pipeline(makePipeline())
    {
    }

    RenderPipeline makePipeline()
    {
        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &library;
        descriptor.sampleCount = sampleCount();
        descriptor.vertexLayout = shader.vertexLayout;

        return Device::shared().makeRenderPipeline(descriptor);
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({Graphics::Color {0.10f, 0.10f, 0.12f}});
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertexBuffer);
        pass.draw(3);
    }

    GeneratedShader shader;
    Buffer vertexBuffer;
    ShaderLibrary library;
    RenderPipeline pipeline;
};

struct MyApp
{
    MyApp() { window.setContentView(triangle); }

    TriangleView triangle;
    Graphics::Window window;
};

int main()
{
    eacp::Apps::run<MyApp>();
    return 0;
}
