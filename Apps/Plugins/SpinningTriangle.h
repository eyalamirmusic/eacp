#pragma once

#include <eacp/GPU/GPU.h>

#include <cmath>

// What both halves of the DemoPlugin pair draw: a clear in the copy's own
// colour with a triangle spun over it. A still of either window then says
// which eacp copy is alive and which one is being pumped - the host's
// triangle stands still, the plugin's only turns while its own Timer fires.
//
// The GPU tier is the whole of what this needs, so the pair builds wherever
// EACP_HAS_GPU does rather than only where the platform has a 2D Context.
namespace eacp::PluginDemo
{
struct Vertex
{
    float position[2];
    float color[3];
};

inline GPU::GeneratedShader makeTriangleShader()
{
    auto builder = GPU::ShaderBuilder {};

    auto position = builder.vertexInput<GPU::Float2>();
    auto color = builder.vertexInput<GPU::Float3>();
    auto varyingColor = builder.varying(color);

    builder.position(GPU::float4(position, 0.f, 1.f));
    builder.fragment(GPU::float4(varyingColor, 1.f));

    return builder.build();
}

struct SpinningTriangleView final : GPU::GPUView
{
    explicit SpinningTriangleView(Graphics::Color backgroundColor)
        : background(backgroundColor)
        , shader(makeTriangleShader())
        , vertexBuffer(GPU::Device::shared().makeBuffer(vertices))
        , library(GPU::Device::shared().makeShaderLibrary(shader.source))
        , pipeline(makePipeline())
    {
        writeVertices();
    }

    GPU::RenderPipeline makePipeline()
    {
        auto descriptor = GPU::RenderPipelineDescriptor {};
        descriptor.library = &library;
        descriptor.sampleCount = sampleCount();
        descriptor.vertexLayout = shader.vertexLayout;

        return GPU::Device::shared().makeRenderPipeline(descriptor);
    }

    void advance()
    {
        angle += 0.05f;
        writeVertices();
        repaint();
    }

    void writeVertices()
    {
        static constexpr float colors[3][3] = {
            {1.f, 0.9f, 0.4f}, {1.f, 1.f, 1.f}, {0.4f, 0.9f, 1.f}};

        for (auto corner = 0; corner < 3; ++corner)
        {
            auto theta = angle + (float) corner * 2.0944f;

            vertices[corner] = {
                {0.7f * std::cos(theta), 0.7f * std::sin(theta)},
                {colors[corner][0], colors[corner][1], colors[corner][2]}};
        }

        // Unordered: rewritten once a frame, which is what this variant is for.
        vertexBuffer.updateUnordered(vertices, (int) sizeof(vertices));
    }

    void render(GPU::Frame& frame) override
    {
        ++framesRendered;

        auto pass = frame.beginPass({background});
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertexBuffer);
        pass.draw(3);
    }

    float angle = 0.f;
    int framesRendered = 0;

    Vertex vertices[3] {};
    Graphics::Color background;
    GPU::GeneratedShader shader;
    GPU::Buffer vertexBuffer;
    GPU::ShaderLibrary library;
    GPU::RenderPipeline pipeline;
};
} // namespace eacp::PluginDemo
