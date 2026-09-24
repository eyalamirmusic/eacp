#include "../X11PluginABI.h"

#include <eacp/Core/Plugins/PluginExport.h>
#include <eacp/Core/Threads/EventLoop-Linux.h>
#include <eacp/GPU/GPU.h>
#include <eacp/Graphics/Window/EmbeddedView.h>

#include <cmath>
#include <memory>

using namespace eacp;

namespace
{
struct Vertex
{
    float position[2];
    float color[3];
};

GPU::GeneratedShader makeShader()
{
    auto builder = GPU::ShaderBuilder {};

    auto position = builder.vertexInput<GPU::Float2>();
    auto color = builder.vertexInput<GPU::Float3>();
    auto varyingColor = builder.varying(color);

    builder.position(GPU::float4(position, 0.f, 1.f));
    builder.fragment(GPU::float4(varyingColor, 1.f));

    return builder.build();
}

// A triangle spun on the CPU and a background that cycles with it: enough
// motion that a still of the host's window says whether the plugin is being
// pumped or not.
struct SpinningView final : GPU::GPUView
{
    SpinningView()
        : shader(makeShader())
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

    // The surface follows the host window until someone calls setBounds on it,
    // so this is what says the parent's ConfigureNotify reached the child.
    void resized() override
    {
        GPU::GPUView::resized();

        auto bounds = getLocalBounds();

        LOG("X11Plugin: content is now ", (int) bounds.w, "x", (int) bounds.h);
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
            {1.f, 0.35f, 0.15f}, {0.15f, 1.f, 0.45f}, {0.3f, 0.45f, 1.f}};

        for (auto corner = 0; corner < 3; ++corner)
        {
            auto theta = angle + (float) corner * 2.0944f;

            vertices[corner] = {
                {0.75f * std::cos(theta), 0.75f * std::sin(theta)},
                {colors[corner][0], colors[corner][1], colors[corner][2]}};
        }

        // Unordered: rewritten once a frame, which is what this variant is for.
        vertexBuffer.updateUnordered(vertices, (int) sizeof(vertices));
    }

    void render(GPU::Frame& frame) override
    {
        ++framesRendered;

        auto wash = 0.10f + 0.06f * std::sin(angle * 0.5f);

        auto pass = frame.beginPass({Graphics::Color {wash, wash * 0.8f, 0.18f}});
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertexBuffer);
        pass.draw(3);
    }

    float angle = 0.f;
    int framesRendered = 0;

    Vertex vertices[3] {};
    GPU::GeneratedShader shader;
    GPU::Buffer vertexBuffer;
    GPU::ShaderLibrary library;
    GPU::RenderPipeline pipeline;
};

// The whole of what a plugin owns: a surface in the host's window, the
// content in it, and the timer that animates it. The timer posts through this
// copy's waker, which is what makes the host's descriptor readable.
struct HostedUI
{
    HostedUI(unsigned long parentWindowId, double scale)
        : view(reinterpret_cast<void*>(parentWindowId))
    {
        view.setPixelsPerPoint((float) scale);
        view.setContentView(content);
    }

    ~HostedUI()
    {
        LOG("X11Plugin: closing after ", content.framesRendered, " frames");
    }

    SpinningView content;
    Graphics::EmbeddedView view;
    Threads::Timer timer {[this] { content.advance(); }, 60};
};

std::unique_ptr<HostedUI> hostedUI;
} // namespace

EACP_PLUGIN_EXPORT int eacp_x11_plugin_open(unsigned long parentWindowId,
                                            double scale)
{
    if (hostedUI != nullptr)
        return 0;

    hostedUI = std::make_unique<HostedUI>(parentWindowId, scale);

    LOG("X11Plugin: opened on window ", parentWindowId, " at scale ", scale);

    return 1;
}

EACP_PLUGIN_EXPORT int eacp_x11_plugin_loop_fd()
{
    return hostedUI != nullptr ? Threads::getEventLoopFd() : -1;
}

EACP_PLUGIN_EXPORT void eacp_x11_plugin_pump()
{
    Threads::pumpEventLoop();
}

EACP_PLUGIN_EXPORT void eacp_x11_plugin_close()
{
    hostedUI.reset();

    // Belt and braces before the host unloads this library: the teardown
    // above leaves nothing deferred that could tick into unmapped code, but
    // anything it did defer runs here rather than never.
    Threads::pumpEventLoop();
}
