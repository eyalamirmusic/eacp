#include <eacp/VideoView/VideoView.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

using namespace eacp;

namespace
{
constexpr auto clipCount = 4;
constexpr auto pollHz = 100;
constexpr auto sweepSwitches = 24;
constexpr auto sweepTicksPerSwitch = 6; // 60 ms at pollHz
constexpr auto readyTimeoutTicks = 1500;
constexpr auto autoplayObserveTicks = 100;
constexpr auto wrapTimeoutTicks = 600;

// A frame counts as advanced when the clock moved by more than this; well
// under a frame interval, well over float noise in the player's clock.
constexpr auto advancedSeconds = 0.05;

// No new decoded frame for half a second while the active clip is playing is a
// stall, not pacing: every clip here is 24fps or better.
constexpr auto freezeTicks = pollHz / 2;

const std::array<const char*, clipCount> clipNames {
    "heavy.mp4", "jellyfish.mp4", "sintel.mp4", "bunny720.mp4"};

// Bundled on macOS, copied next to the executable on Windows.
FilePath resolveClip(const std::string& name)
{
    auto bundled = Files::getBundleResourcePath(name);

    if (!bundled.empty() && File {FilePath {bundled}}.exists())
        return FilePath {bundled};

    return FilePath {"media/" + name};
}

Graphics::WindowOptions makeOptions()
{
    auto options = Graphics::WindowOptions {};
    options.width = 1280;
    options.height = 720;
    options.title = "eacp Playing Heavy Content";
    options.flags = {Graphics::WindowFlags::Titled,
                     Graphics::WindowFlags::Closable,
                     Graphics::WindowFlags::Miniaturizable,
                     Graphics::WindowFlags::Resizable};
    return options;
}

struct Check
{
    const char* name;
    bool resolved = false;
    bool passed = false;
};

// The whole demo is one GPUView: four players, four presenters, and the HUD
// all composite in a single render pass on one device. That is the point of
// the app — the layered arrangement hub's glyph background needs, without a
// native view per video.
struct HeavyContentView final : GPU::GPUView
{
    enum class Phase
    {
        WaitReady,
        Autoplay,
        Sweep,
        LoopWrap,
        Done
    };

    HeavyContentView()
    {
        // Video is continuous-tone content; MSAA buys nothing.
        setSampleCount(1);
        setContinuous(true);

        // Without this the view is invisible to hitTest, so hovering a tile
        // never reaches mouseMoved.
        setHandlesMouseEvents(true);

        openClips();
    }

    void openClips()
    {
        for (auto index = 0; index < clipCount; ++index)
        {
            auto& player = players[(std::size_t) index];
            auto path = resolveClip(clipNames[(std::size_t) index]);

            player.onError =
                [name = clipNames[(std::size_t) index]](const std::string& message)
            { std::printf("%s: %s\n", name, message.c_str()); };

            player.setLooping(true);
            player.setMuted(true);

            if (!player.open(path))
                std::printf("could not open %s\n", path.str().c_str());

            // Every clip runs from launch and never stops; switching only
            // changes which one is on the stage.
            player.play();
        }
    }

    // Layout -----------------------------------------------------------------

    Graphics::Rect tileRect(const Graphics::Rect& bounds, int index) const
    {
        auto margin = 16.0f;
        auto gap = 12.0f;
        auto tileHeight = 108.0f;
        auto barHeight = 6.0f;
        auto barGap = 6.0f;

        auto width =
            (bounds.w - margin * 2.0f - gap * (clipCount - 1)) / (float) clipCount;
        auto top = bounds.h - margin - barHeight - barGap - tileHeight;

        return {margin + (width + gap) * (float) index, top, width, tileHeight};
    }

    Graphics::Rect progressRect(const Graphics::Rect& tile) const
    {
        return {tile.x, tile.y + tile.h + 6.0f, tile.w, 6.0f};
    }

    // Render -----------------------------------------------------------------

    void ensureRenderer()
    {
        auto bounds = getLocalBounds();
        auto size = Graphics::Point {bounds.w, bounds.h};

        if (!renderer.has_value() || size.x != rendererSize.x
            || size.y != rendererSize.y)
        {
            renderer.emplace(size, sampleCount());
            rendererSize = size;
        }
    }

    void render(GPU::Frame& frame) override
    {
        ensureRenderer();

        auto pass = frame.beginPass({Graphics::Color::black()});
        renderer->begin(pass);

        auto bounds = getLocalBounds();
        auto activeIndex = (std::size_t) active;

        auto stageArea = stagePresenters[activeIndex].draw(
            players[activeIndex], *renderer, bounds, Video::Fit::Cover);

        if (countingStalls && isReady(active) && stageArea.isEmpty())
            ++stalls;

        drawTiles(bounds);
        ++renderTicks;
    }

    void drawTiles(const Graphics::Rect& bounds)
    {
        for (auto index = 0; index < clipCount; ++index)
        {
            auto slot = (std::size_t) index;
            auto tile = tileRect(bounds, index);

            renderer->fillRect(tile, {0.05f, 0.05f, 0.07f, 0.85f});
            tilePresenters[slot].draw(
                players[slot], *renderer, tile.inset(2.0f), Video::Fit::Cover);

            auto ready = isReady(index);
            auto border = ready ? Graphics::Color {0.2f, 1.0f, 0.45f, 0.9f}
                                : Graphics::Color {1.0f, 0.75f, 0.2f, 0.9f};

            renderer->drawRect(tile, border, 2.0f);

            if (index == active)
                renderer->drawRect(
                    tile.inset(-3.0f), {1.0f, 1.0f, 1.0f, 0.95f}, 3.0f);

            drawProgress(progressRect(tile), index);
        }
    }

    void drawProgress(const Graphics::Rect& bar, int index)
    {
        renderer->fillRect(bar, {1.0f, 1.0f, 1.0f, 0.18f});

        const auto& player = players[(std::size_t) index];
        auto duration = player.duration();

        if (duration <= 0.0)
            return;

        auto fraction = (float) (player.currentTime() / duration);
        fraction = std::clamp(fraction, 0.0f, 1.0f);

        renderer->fillRect(bar.withWidth(bar.w * fraction),
                           {0.35f, 0.8f, 1.0f, 0.95f});
    }

    // Interaction ------------------------------------------------------------

    void mouseMoved(const Graphics::MouseEvent& event) override
    {
        // The sweep owns the stage while it runs, so hover cannot perturb the
        // measurement it is taking.
        if (phase == Phase::Sweep)
            return;

        auto bounds = getLocalBounds();

        for (auto index = 0; index < clipCount; ++index)
            if (tileRect(bounds, index).contains(event.pos))
                setActive(index);
    }

    void setActive(int index)
    {
        if (index == active)
            return;

        active = index;
        ++switches;
        sinceNewFrame = 0;
        lastSequence = players[(std::size_t) active].frameSequence();
    }

    // Checks -----------------------------------------------------------------

    bool isReady(int index) const
    {
        return players[(std::size_t) index].state() == Video::PlayerState::Ready;
    }

    bool isLoaded(int index) const
    {
        const auto& player = players[(std::size_t) index];
        return isReady(index) && player.width() > 0 && player.height() > 0
               && player.duration() > 0.0;
    }

    void resolve(Check& check, bool passed, const std::string& detail)
    {
        check.resolved = true;
        check.passed = passed;
        std::printf(
            "[%s] %s — %s\n", passed ? "PASS" : "FAIL", check.name, detail.c_str());
    }

    void tick()
    {
        ++phaseTicks;
        detectFreeze();

        switch (phase)
        {
            case Phase::WaitReady:
                tickWaitReady();
                break;
            case Phase::Autoplay:
                tickAutoplay();
                break;
            case Phase::Sweep:
                tickSweep();
                break;
            case Phase::LoopWrap:
                tickLoopWrap();
                break;
            case Phase::Done:
                break;
        }
    }

    // A stall the empty-rect test cannot see: the stage keeps drawing, but the
    // same decoded frame over and over.
    void detectFreeze()
    {
        if (!countingStalls || !isReady(active))
        {
            sinceNewFrame = 0;
            return;
        }

        auto sequence = players[(std::size_t) active].frameSequence();

        if (sequence != lastSequence)
        {
            lastSequence = sequence;
            sinceNewFrame = 0;
            return;
        }

        if (++sinceNewFrame >= freezeTicks)
        {
            ++stalls;
            sinceNewFrame = 0;
        }
    }

    void enterPhase(Phase next)
    {
        phase = next;
        phaseTicks = 0;
    }

    void tickWaitReady()
    {
        auto loaded = 0;

        for (auto index = 0; index < clipCount; ++index)
            if (isLoaded(index))
                ++loaded;

        if (loaded < clipCount && phaseTicks < readyTimeoutTicks)
            return;

        resolve(checks[0],
                loaded == clipCount,
                std::to_string(loaded) + "/" + std::to_string(clipCount)
                    + " clips Ready with dimensions and duration");

        for (auto index = 0; index < clipCount; ++index)
            startTimes[(std::size_t) index] =
                players[(std::size_t) index].currentTime();

        enterPhase(Phase::Autoplay);
    }

    void tickAutoplay()
    {
        if (phaseTicks < autoplayObserveTicks)
            return;

        auto activeAdvanced = advanced(active);
        resolve(checks[1],
                activeAdvanced,
                std::string("active clip ") + clipNames[(std::size_t) active]
                    + (activeAdvanced ? " advanced" : " did not advance")
                    + " with no interaction");

        auto advancing = 0;

        for (auto index = 0; index < clipCount; ++index)
            if (advanced(index))
                ++advancing;

        resolve(checks[2],
                advancing == clipCount,
                std::to_string(advancing) + "/" + std::to_string(clipCount)
                    + " clips advanced while playing simultaneously");

        countingStalls = true;
        stalls = 0;
        sinceNewFrame = 0;
        lastSequence = players[(std::size_t) active].frameSequence();

        enterPhase(Phase::Sweep);
    }

    bool advanced(int index) const
    {
        auto slot = (std::size_t) index;
        return players[slot].currentTime() - startTimes[slot] > advancedSeconds;
    }

    void tickSweep()
    {
        if (phaseTicks % sweepTicksPerSwitch != 0)
            return;

        if (sweepDone < sweepSwitches)
        {
            // setActive() is bypassed on purpose: the sweep drives the same
            // switch the hover does, and is counted the same way.
            active = (active + 1) % clipCount;
            ++switches;
            sinceNewFrame = 0;
            lastSequence = players[(std::size_t) active].frameSequence();
            ++sweepDone;
            return;
        }

        resolve(checks[3],
                stalls == 0,
                std::to_string(sweepSwitches) + " switches @ 60 ms, "
                    + std::to_string(stalls) + " stalls");

        // A 10-second clip would take 10 seconds to wrap on its own; seek to
        // the tail and watch the wrap the loop does.
        auto& player = players[(std::size_t) active];
        auto duration = player.duration();

        if (duration > 1.0)
            player.seek(duration - 0.4);

        wrapReference = player.currentTime();
        enterPhase(Phase::LoopWrap);
    }

    void tickLoopWrap()
    {
        auto& player = players[(std::size_t) active];
        auto now = player.currentTime();

        if (now > wrapReference)
            wrapReference = now;

        // The clock going backwards is the wrap: nothing else moves it that way.
        auto wrapped = now < wrapReference - 0.5;

        if (!wrapped && phaseTicks < wrapTimeoutTicks)
            return;

        resolve(checks[4],
                wrapped,
                wrapped ? "loop wrap observed on "
                              + std::string(clipNames[(std::size_t) active])
                        : "no loop wrap within "
                              + std::to_string(wrapTimeoutTicks / pollHz) + "s");

        report();
        enterPhase(Phase::Done);
    }

    void report()
    {
        auto allPassed = true;

        for (const auto& check: checks)
            if (!check.passed)
                allPassed = false;

        if (allPassed)
            std::printf("ALL CHECKS PASSED — %d hover switches, %d stalls\n",
                        switches,
                        stalls);
        else
            std::printf(
                "CHECKS FAILED — %d hover switches, %d stalls\n", switches, stalls);

        std::fflush(stdout);
        finished = true;
    }

    // The checks measure the display path through the player and the presenter;
    // this writes out what the pass actually produced, for a machine with no
    // eyes on the window.
    void saveSnapshot(const FilePath& path)
    {
        auto image = renderToImage();

        if (!image.isValid())
        {
            std::printf("snapshot failed: the view produced no image\n");
            return;
        }

        image.save(path);
        std::printf("snapshot written to %s (%dx%d)\n",
                    path.str().c_str(),
                    image.width(),
                    image.height());
    }

    std::array<Video::Player, clipCount> players;

    // A presenter is bound to one player: its CPU-upload scratch is gated on
    // that player's frame sequence, so sharing one across players would read a
    // stale frame after a switch on the upload path.
    std::array<Video::FramePresenter, clipCount> stagePresenters;
    std::array<Video::FramePresenter, clipCount> tilePresenters;

    std::optional<Sprites::SpriteRenderer> renderer;
    Graphics::Point rendererSize {0.0f, 0.0f};

    std::array<Check, 5> checks {
        Check {"All clips reach a playable state"},
        Check {"A clip auto-plays on launch (no interaction)"},
        Check {"Every clip advances while playing simultaneously"},
        Check {"Fast sweep (24 switches @ 60 ms) with no stalls"},
        Check {"A looping clip wraps"}};

    Phase phase = Phase::WaitReady;
    int phaseTicks = 0;
    int active = 0;
    int switches = 0;
    int stalls = 0;
    int sweepDone = 0;
    int renderTicks = 0;
    int sinceNewFrame = 0;
    std::uint64_t lastSequence = 0;
    bool countingStalls = false;
    bool finished = false;
    double wrapReference = 0.0;
    std::array<double, clipCount> startTimes {};
};

struct HeavyContentApp
{
    HeavyContentApp()
    {
        window.setContentView(view);
        armAutoQuit();
    }

    // Timer-driven, not render-driven, so it fires even if the display path
    // never produces a frame.
    void armAutoQuit()
    {
        autoQuitSeconds =
            std::atof(getEnvValue("EACP_DEMO_AUTOQUIT_SECONDS").c_str());

        if (autoQuitSeconds > 0.0)
            deadline.emplace(Time::MS {(std::int64_t) (autoQuitSeconds * 1000.0)});
    }

    void tick()
    {
        view.tick();

        if (autoQuitSeconds <= 0.0)
        {
            // Interactive run: once the checks have reported, the poll has
            // nothing left to measure — hovering and rendering are driven by
            // events and the display link, not this timer.
            if (view.finished)
                poll.stop();

            return;
        }

        // Quit as soon as the checks have reported, so a headless verification
        // run does not idle out its whole budget; the deadline is the backstop
        // for a run that never gets there.
        if (view.finished || deadline->expired())
        {
            auto snapshotPath = getEnvValue("EACP_DEMO_SNAPSHOT_PATH");

            if (!snapshotPath.empty())
                view.saveSnapshot(FilePath {snapshotPath});

            if (!view.finished)
                std::printf("auto-quit before the checks finished — %d renders, "
                            "%d switches\n",
                            view.renderTicks,
                            view.switches);

            Apps::quit();
        }
    }

    HeavyContentView view;
    Graphics::Window window {makeOptions()};
    double autoQuitSeconds = 0.0;
    std::optional<Time::Deadline> deadline;
    Threads::Timer poll {[this] { tick(); }, pollHz};
};
} // namespace

int main()
{
    return eacp::Apps::run<HeavyContentApp>();
}
