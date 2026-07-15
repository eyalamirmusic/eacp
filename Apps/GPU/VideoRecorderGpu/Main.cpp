#include <eacp/GPU/GPU.h>
#include <eacp/Video/VideoRecorder.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace eacp;
using namespace GPU;

namespace
{
std::string outputPath()
{
    const auto* home = std::getenv("HOME");
    auto dir = home != nullptr ? std::filesystem::path(home) / "Downloads"
                               : std::filesystem::temp_directory_path();

    return (dir / "eacp-recording.mp4").string();
}

const char* triangleShader = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn { float2 position [[attribute(0)]]; };

vertex float4 vertexMain(VertexIn in [[stage_in]])
{
    return float4(in.position, 0.0, 1.0);
}

fragment float4 fragmentMain() { return float4(1.0, 1.0, 1.0, 1.0); }
)";
} // namespace

// A white triangle over a clear colour that cycles with `phase`. Recording it in
// GpuDirect mode proves each frame reflects live GPU state -- the background
// colour differs frame to frame.
struct SpinView final : GPUView
{
    SpinView()
        : vertexBuffer(Device::shared().makeBuffer(triangle))
        , library(
              Device::shared().makeShaderLibrary(ShaderSource::msl(triangleShader)
                                                     .withVertex("vertexMain")
                                                     .withFragment("fragmentMain")))
        , pipeline(makePipeline())
    {
    }

    RenderPipeline makePipeline()
    {
        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &library;
        descriptor.sampleCount = sampleCount();
        descriptor.vertexLayout.attribute(VertexFormat::Float2, 0);
        descriptor.vertexLayout.stride = sizeof(float) * 2;

        return Device::shared().makeRenderPipeline(descriptor);
    }

    void render(Frame& frame) override
    {
        auto pass =
            frame.beginPass({Graphics::Color {phase, 0.25f, 1.0f - phase, 1.f}});
        pass.setPipeline(pipeline);
        pass.setVertexBuffer(vertexBuffer);
        pass.draw(3);
    }

    static constexpr float triangle[] = {0.f, 0.7f, -0.7f, -0.7f, 0.7f, -0.7f};

    float phase = 0.f; // 0..1
    Buffer vertexBuffer;
    ShaderLibrary library;
    RenderPipeline pipeline;
};

struct App
{
    App() { window.setContentView(view); }

    void tick(Threads::FrameTime time)
    {
        view.phase = (float) (std::fmod(time.time, 2.0) / 2.0);
        view.repaint();

        if (!started && time.time > 0.3)
        {
            path = outputPath();

            auto options = Video::VideoOptions {};
            options.mode = Video::CaptureMode::GpuDirect;

            started = recorder.start(view, path, options);
            startTime = time.time;
            LOG(started ? "GpuDirect capture -> " + path
                        : std::string("GpuDirect start FAILED"));
        }

        if (started && !stopping && (time.time - startTime) >= 2.0)
        {
            stopping = true;
            recorder.stop().then(
                [this]
                {
                    LOG("recording finished: " + path);
                    Apps::quit();
                });
        }
    }

    SpinView view;
    Video::VideoRecorder recorder;

    Graphics::WindowOptions options = []
    {
        auto o = Graphics::WindowOptions();
        o.width = 240;
        o.height = 160;
        return o;
    }();
    Graphics::Window window {options};

    bool started = false;
    bool stopping = false;
    double startTime = 0.0;
    std::string path;

    Threads::DisplayLink link {[this](Threads::FrameTime time) { tick(time); }};
};

int main()
{
    return eacp::Apps::run<App>();
}
