#include <eacp/Text/TextRenderer.h>
#include <eacp/Video/SyntheticClip.h>
#include <eacp/VideoView/VideoView.h>

#include <cstdarg>
#include <cstdio>

using namespace eacp;

namespace
{
constexpr auto windowWidth = 960;
constexpr auto windowHeight = 600;

// The scrub bar's strip along the bottom of the view, in logical points.
constexpr auto scrubHeight = 28.0f;
constexpr auto scrubInset = 16.0f;

// Ten seconds of 1080p30, which is a real decode load rather than a token one.
Video::SyntheticClipOptions clipOptions()
{
    auto options = Video::SyntheticClipOptions {};
    options.width = 1920;
    options.height = 1080;
    options.fps = 30;
    options.duration = 10.0;
    return options;
}

// A path on the command line if given — that is how to point the sample at real
// heavy content — otherwise a generated clip, encoded once into the user's
// cache directory and reused from then on. Nothing is read from the source
// tree, so no media has to be committed to run this.
FilePath resolveVideoPath()
{
    const auto& args = Apps::getAppEnvironment().commandLineArgs;

    if (args.size() > 1)
        return FilePath {args[1]};

    logMessage("No file given — generating a synthetic 1080p clip (first run "
               "only; cached afterwards).");

    return Video::cachedSyntheticClip(clipOptions());
}

std::string formatted(const char* format, ...)
{
    char buffer[256] = {};

    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    return buffer;
}

Graphics::WindowOptions windowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = windowWidth;
    options.height = windowHeight;
    options.title = "eacp Video Playback";
    return options;
}
} // namespace

// A player view with a scrub bar and a stats HUD.
//
// The scrub bar is the point of the sample: the same stream is driven two ways.
// Normally the attached Player runs the clock off the display link; while the
// bar is being dragged the app takes the playhead over with setPosition,
// exactly as an editing timeline would.
//
// The HUD is there because "does this keep up" is not a question you can answer
// by looking at the picture. Skipped frames rising while the queue sits empty
// means decode is the bottleneck; skipped rising with a full queue means
// presentation is.
struct PlaybackView final : Video::VideoView
{
    PlaybackView() { setHandlesMouseEvents(true); }

    // Unhooks from the stream while it is still alive. The base destructor
    // cannot do this: by the time it runs, `stream` below has already gone.
    ~PlaybackView() override { detach(); }

    Graphics::Rect scrubArea() const
    {
        auto bounds = getLocalBounds();
        return {scrubInset,
                bounds.h - scrubHeight - scrubInset,
                bounds.w - scrubInset * 2.0f,
                scrubHeight};
    }

    void update(Threads::FrameTime frameTime) override
    {
        VideoView::update(frameTime);

        // Exponentially smoothed, so the number is readable rather than
        // flickering with every refresh.
        if (frameTime.delta > 0.0)
        {
            auto instant = 1.0 / frameTime.delta;
            smoothedFps =
                smoothedFps > 0.0 ? smoothedFps * 0.9 + instant * 0.1 : instant;
        }
    }

    void drawScrubBar(Sprites::SpriteRenderer& renderer)
    {
        auto area = scrubArea();
        auto duration = player.stream().info().duration;
        auto progress =
            duration > 0.0 ? (float) (player.position() / duration) : 0.0f;

        renderer.fillRect(area, {0.0f, 0.0f, 0.0f, 0.45f});

        auto played = area;
        played.w = area.w * progress;
        renderer.fillRect(played, {0.25f, 0.7f, 1.0f, 0.9f});

        renderer.drawRect(area, {1.0f, 1.0f, 1.0f, 0.35f}, 1.0f);

        // A dot on the playhead, so the position is readable even when the
        // played portion is too short to see.
        auto dot = Graphics::Rect {
            area.x + area.w * progress - 3.0f, area.y - 2.0f, 6.0f, area.h + 4.0f};
        renderer.fillRect(dot, {1.0f, 1.0f, 1.0f, 0.95f});
    }

    void drawHud(GPU::RenderPass& pass, Sprites::SpriteRenderer& renderer)
    {
        const auto& info = player.stream().info();
        auto stats = player.stream().stats();

        const std::string lines[] = {
            formatted("%dx%d  %.0f fps  %.1fs%s",
                      info.width,
                      info.height,
                      info.frameRate,
                      info.duration,
                      info.rotationDegrees != 0
                          ? formatted("  rot %d", info.rotationDegrees).c_str()
                          : ""),
            formatted("render  %.1f fps", smoothedFps),
            formatted("decoded %llu   skipped %llu",
                      (unsigned long long) stats.decoded,
                      (unsigned long long) stats.skipped),
            formatted("queue   %d/%d", stats.queued, stats.depth),
            formatted("upload  %s",
                      lastFrameWasZeroCopy() ? "zero-copy" : "cpu copy"),
            formatted("%.2f / %.2f s  %s",
                      player.position(),
                      info.duration,
                      player.isPlaying() ? "playing" : "paused"),
        };

        auto bounds = getLocalBounds();
        hud.setViewport({bounds.w, bounds.h}, backingScale());
        hud.begin();

        auto lineStep = hud.lineHeight();
        auto widest = 0.0f;

        for (const auto& line: lines)
            widest = std::max(widest, hud.measure(line));

        constexpr auto padding = 8.0f;
        auto panel =
            Graphics::Rect {scrubInset,
                            scrubInset,
                            widest + padding * 2.0f,
                            lineStep * (float) std::size(lines) + padding * 2.0f};

        renderer.fillRect(panel, {0.0f, 0.0f, 0.0f, 0.55f});
        renderer.drawRect(panel, {1.0f, 1.0f, 1.0f, 0.2f}, 1.0f);

        auto baseline = panel.y + padding + hud.ascent();

        for (const auto& line: lines)
        {
            hud.draw(line, {panel.x + padding, baseline}, {0.85f, 0.95f, 1.0f});
            baseline += lineStep;
        }

        // After the sprite draws, so the text lands on top of the panel.
        hud.flush(pass);
    }

    void drawOverlay(GPU::RenderPass& pass,
                     Sprites::SpriteRenderer& renderer,
                     const Graphics::Rect&) override
    {
        drawScrubBar(renderer);
        drawHud(pass, renderer);
    }

    void mouseDown(const Graphics::MouseEvent& event) override
    {
        if (scrubArea().contains(event.pos))
        {
            scrubbing = true;
            wasPlaying = player.isPlaying();
            player.pause();
            scrubTo(event.pos.x);
            return;
        }

        if (player.isPlaying())
            player.pause();
        else
            player.play();
    }

    void mouseDragged(const Graphics::MouseEvent& event) override
    {
        if (scrubbing)
            scrubTo(event.pos.x);
    }

    void mouseUp(const Graphics::MouseEvent&) override
    {
        if (!scrubbing)
            return;

        scrubbing = false;

        if (wasPlaying)
            player.play();
    }

    void scrubTo(float x)
    {
        auto area = scrubArea();
        auto duration = player.stream().info().duration;

        if (area.w <= 0.0f || duration <= 0.0)
            return;

        auto fraction = std::clamp((x - area.x) / area.w, 0.0f, 1.0f);
        player.setPosition(duration * fraction);
        repaint();
    }

    Video::FrameStream stream;
    Video::Player player {stream};
    Text::TextRenderer hud {12.0f};

    double smoothedFps = 0.0;
    bool scrubbing = false;
    bool wasPlaying = false;
};

struct PlaybackApp
{
    PlaybackApp()
    {
        auto path = resolveVideoPath();

        if (path.empty() || !view.stream.open(path))
        {
            logMessage("Could not open video: " + path.str());
            Apps::quit(1);
            return;
        }

        view.setFit(Video::VideoView::Fit::Contain);
        view.attach(view.player);
        view.player.setLooping(true);
        view.player.play();

        window.setContentView(view);
    }

    PlaybackView view;
    Graphics::Window window {windowOptions()};
};

int main(int argc, char* argv[])
{
    return eacp::Apps::run<PlaybackApp>(argc, argv);
}
