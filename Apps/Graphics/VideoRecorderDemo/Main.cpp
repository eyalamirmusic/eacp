#include <eacp/Graphics/Graphics.h>
#include <eacp/Video/VideoRecorder.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace eacp;
using namespace Graphics;

namespace
{
// A stable path in ~/Downloads so the clip is easy to find; the recorder deletes
// any existing file there first, so only one ever accumulates.
std::string outputPath()
{
    const auto* home = std::getenv("HOME");
    auto dir = home != nullptr ? std::filesystem::path(home) / "Downloads"
                               : std::filesystem::temp_directory_path();

    return (dir / "eacp-recording.mp4").string();
}
} // namespace

// A dark view with a red box sliding left-to-right, driven by a display link, so
// the recorded video shows visible motion.
struct Animated final : View
{
    void paint(Context& g) override
    {
        auto bounds = getLocalBounds();

        g.setColor({0.1f, 0.1f, 0.12f, 1.f});
        g.fillRect(bounds);

        auto boxSize = 40.f;
        auto x = phase * (bounds.w - boxSize);
        g.setColor({0.95f, 0.3f, 0.2f, 1.f});
        g.fillRect({x, bounds.h / 2.f - boxSize / 2.f, boxSize, boxSize});
    }

    float phase = 0.f; // 0..1
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
            const auto* modeEnv = std::getenv("EACP_CAPTURE");
            auto screen = modeEnv != nullptr && std::string(modeEnv) == "screen";
            options.mode =
                screen ? Video::CaptureMode::Screen : Video::CaptureMode::Snapshot;

            started = recorder.start(view, path, options);
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

    Animated view;
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
