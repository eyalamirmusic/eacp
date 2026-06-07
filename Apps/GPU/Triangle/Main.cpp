#include <eacp/Graphics/Graphics.h>
#include <eacp/GPU/GPU.h>

#include <eacp/Core/Platform/Platform.h>

#include <ResEmbed/ResEmbed.h>

#include <stdexcept>
#include <string>

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

ShaderSource loadTriangleShader()
{
    auto fileName = Platform::isWindows() ? "Triangle.hlsl" : "Triangle.metal";

    auto shader = ResEmbed::get(fileName, "TriangleShaders");

    if (!shader)
        throw std::runtime_error(std::string("Triangle: embedded ") + fileName
                                 + " not found");

    auto source = Platform::isWindows() ? ShaderSource::hlsl(shader.toString())
                                        : ShaderSource::msl(shader.toString());

    return source.withVertex("vertexMain").withFragment("fragmentMain");
}
} // namespace

struct TriangleView final : GPUView
{
    TriangleView()
        : vertexBuffer(Device::shared().makeBuffer(triangleVertices))
        , library(Device::shared().makeShaderLibrary(loadTriangleShader()))
        , pipeline(makePipeline())
    {
    }

    RenderPipeline makePipeline()
    {
        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &library;
        descriptor.sampleCount = sampleCount();
        descriptor.vertexLayout.attribute(VertexFormat::Float2, 0)
            .attribute(VertexFormat::Float3, sizeof(float) * 2);
        descriptor.vertexLayout.stride = sizeof(Vertex);

        return Device::shared().makeRenderPipeline(descriptor);
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({Graphics::Color {0.10f, 0.10f, 0.12f}});
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertexBuffer);
        pass.draw(3);
    }

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
