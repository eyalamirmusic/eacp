#include <eacp/Core/Utils/Environment.h>
#include <eacp/UI/UI.h>
#include <eacp/Video/VideoRecorder.h>

#include <cmath>
#include <string>

using namespace eacp;
using namespace Graphics;

namespace
{
// The recorder deletes any existing file at this path first, so only one clip
// ever accumulates.
FilePath outputPath()
{
    auto dir = FilePath::downloadsDirectory();
    if (dir.empty())
        dir = FilePath::tempDirectory();

    return dir / "eacp-recording.mp4";
}
} // namespace

struct Animated final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        auto bounds = getLocalBounds();

        g.fillAll({0.1f, 0.1f, 0.12f, 1.f});

        auto boxSize = 40.f;
        auto x = phase * (bounds.w - boxSize);

        g.setColour({0.95f, 0.3f, 0.2f, 1.f});
        g.fillRect({x, bounds.h / 2.f - boxSize / 2.f, boxSize, boxSize});
    }

    float phase = 0.f; // 0..1
};

struct Host final : UI::ComponentHost
{
    Host()
    {
        setBackgroundColour({0.1f, 0.1f, 0.12f, 1.f});
        setRootComponent(animated);
    }

    Animated animated;
};

struct App
{
    App() { window.setContentView(host); }

    void tick(Threads::FrameTime time)
    {
        host.animated.phase = (float) (std::fmod(time.time, 2.0) / 2.0);
        host.animated.repaint();

        if (!started && time.time > 0.3)
        {
            path = outputPath().str();

            auto options = Video::VideoOptions {};
            auto screen = getEnvValue("EACP_CAPTURE") == "screen";
            options.mode =
                screen ? Video::CaptureMode::Screen : Video::CaptureMode::Snapshot;

            started = recorder.start(host, path, options);
            startTime = time.time;

            LOG(std::string(screen ? "screen" : "snapshot") + " capture -> " + path);
            if (!started)
                LOG("recording start FAILED");
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

    Host host;
    Video::VideoRecorder recorder;

    WindowOptions options = []
    {
        auto o = WindowOptions();
        o.width = 240;
        o.height = 160;
        return o;
    }();
    Window window {options};

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
