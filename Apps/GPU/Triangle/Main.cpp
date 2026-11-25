#include <eacp/GPU/GPU.h>

#include <ResEmbed/ResEmbed.h>

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

std::string embeddedShader(const char* fileName)
{
    auto shader = ResEmbed::get(fileName, "TriangleShaders");
    return shader ? shader.toString() : std::string {};
}

ShaderSource loadTriangleShader()
{
    auto shader = ShaderSource::native({embeddedShader("Triangle.metal"),
                                        embeddedShader("Triangle.hlsl"),
                                        embeddedShader("Triangle.glsl"),
                                        embeddedShader("Triangle.wgsl")});

    if (shader.source.empty())
        throw std::runtime_error("Triangle: no embedded shader for this backend");

    return shader.withVertex("vertexMain").withFragment("fragmentMain");
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

int main()
{
    return Graphics::runWindowedApp<TriangleView>();
}
