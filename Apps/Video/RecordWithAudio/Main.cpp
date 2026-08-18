// Recording a view and an audio track into one file. eacp has no audio source
// of its own -- the app owns the sound -- so the recorder takes blocks pushed
// in from whatever thread produces them, here a stand-in device thread.
//
// The clip is also built to be measured. The box is against a wall on every
// whole second and a blip sounds at exactly that moment, so the gap between the
// two in the finished file is the capture path's latency, in a form a frame
// step or an audio editor reads straight off. What it will NOT show is drift:
// the two media are stamped on one clock, so the gap stays put -- ~40 ms for
// the snapshot tier, more for a screen-captured window, whose pixels are as old
// as the compositor's copy of them. Feed that number back as
// AudioSpec::latencyFrames and the file lines up.
//
// EACP_CAPTURE=screen switches tiers.

#include <eacp/Core/Utils/Environment.h>
#include <eacp/UI/UI.h>
#include <eacp/Video/VideoRecorder.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

using namespace eacp;
using namespace Graphics;

namespace
{
constexpr auto twoPi = 6.283185307179586;

// The box crosses the window and comes back every two seconds, so it is against
// a wall on every whole second -- which is where the blip below lands. Picture
// and sound each read the time and work out their own state from it, never from
// each other, so a file whose blip does not fall on the frame the box turns is
// out of sync, and by how much is countable in frames.
constexpr auto bouncePeriod = 2.0;

// The one origin both sides measure from. The display link's own clock starts
// at its first tick, which is a window creation later than the tone thread
// starts, so neither side may use a zero of its own.
class Clock
{
public:
    double elapsed() const
    {
        auto since = std::chrono::steady_clock::now() - start;
        return std::chrono::duration<double>(since).count();
    }

private:
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

double bouncePhase(double seconds)
{
    auto phase = std::fmod(seconds, bouncePeriod) / (bouncePeriod / 2.0);
    return phase < 1.0 ? phase : 2.0 - phase;
}

float blipSample(double seconds)
{
    auto sinceBlip = std::fmod(seconds, bouncePeriod / 2.0);
    auto envelope = std::exp(-sinceBlip * 25.0);

    return (float) (0.3 * envelope * std::sin(twoPi * 880.0 * sinceBlip));
}

bool wantsScreenTier()
{
    return getEnvValue("EACP_CAPTURE") == "screen";
}

FilePath outputPath()
{
    auto dir = FilePath::downloadsDirectory();
    if (dir.empty())
        dir = FilePath::tempDirectory();

    return dir / "eacp-recording-with-audio.mp4";
}

// Stands in for an audio device: a thread producing exactly as many frames as
// real time has called for, in blocks, on its own clock -- which is the shape
// pushAudio is written for. A plugin or a host engine pushes from its callback
// instead; nothing else changes.
class ToneDevice
{
public:
    ToneDevice(Video::VideoRecorder& recorderToUse,
               const Video::AudioSpec& specToUse,
               const Clock& clockToUse)
        : recorder(recorderToUse)
        , spec(specToUse)
        , clock(clockToUse)
    {
        samples.resize(spec.numChannels * blockFrames);
        channels.resize(spec.numChannels);

        for (auto channel = 0; channel < spec.numChannels; ++channel)
            channels[channel] = samples.data() + channel * blockFrames;
    }

    ~ToneDevice() { stop(); }

    void start()
    {
        running = true;
        thread = std::thread([this] { run(); });
    }

    void stop()
    {
        running = false;

        if (thread.joinable())
            thread.join();
    }

private:
    void run()
    {
        auto produced = std::int64_t {0};

        while (running)
        {
            auto due = (std::int64_t) (clock.elapsed() * spec.sampleRate);

            if (due - produced < blockFrames)
            {
                Time::sleepMS(2);
                continue;
            }

            for (auto frame = 0; frame < blockFrames; ++frame)
            {
                auto value =
                    blipSample((double) (produced + frame) / spec.sampleRate);

                for (auto channel = 0; channel < spec.numChannels; ++channel)
                    samples[channel * blockFrames + frame] = value;
            }

            recorder.pushAudio({channels.data(), spec.numChannels, blockFrames});
            produced += blockFrames;
        }
    }

    static constexpr auto blockFrames = 256;

    Video::VideoRecorder& recorder;
    Video::AudioSpec spec;
    const Clock& clock;
    Vector<float> samples;
    Vector<const float*> channels;
    std::atomic<bool> running {false};
    std::thread thread;
};

struct Bouncing final : UI::Component
{
    void paint(UI::Graphics& g) override
    {
        auto bounds = getLocalBounds();

        g.fillAll({0.1f, 0.1f, 0.12f, 1.f});

        auto boxSize = 40.f;
        auto x = (float) phase * (bounds.w - boxSize);

        g.setColour({0.95f, 0.3f, 0.2f, 1.f});
        g.fillRect({x, bounds.h / 2.f - boxSize / 2.f, boxSize, boxSize});
    }

    double phase = 0.0;
};

struct Host final : UI::ComponentHost
{
    Host()
    {
        setBackgroundColour({0.1f, 0.1f, 0.12f, 1.f});
        setRootComponent(bouncing);
    }

    Bouncing bouncing;
};
} // namespace

struct App
{
    App()
    {
        window.setContentView(host);

        // The animation is what the recording is measured against, so it is
        // paced to the rate being captured rather than to the panel: repainting
        // at 120 Hz into a 60 Hz capture leaves the compositor a frame behind
        // and the file reads as if the sound were early.
        link.setMaxFps(60);

        // Asked for up front, because the answer only reaches the next launch:
        // the grant a first-time user gives here applies to the run after this
        // one, so the recording below still fails and says why.
        if (wantsScreenTier() && !Video::hasScreenCapturePermission())
            Video::requestScreenCapturePermission(
                [](bool granted)
                {
                    LOG(granted ? "screen recording permitted"
                                : "screen recording not permitted -- grant it in "
                                  "System Settings and relaunch");
                });

        device.start();
    }

    void tick()
    {
        auto now = clock.elapsed();

        host.bouncing.phase = bouncePhase(now);
        host.bouncing.repaint();

        if (!started && now > 0.5)
            startRecording(now);

        if (started && !stopping && (now - startTime) >= 4.0)
            stopRecording();
    }

    void startRecording(double now)
    {
        path = outputPath().str();

        auto options = Video::RecordingOptions {};
        auto screen = wantsScreenTier();
        options.mode =
            screen ? Video::CaptureMode::Screen : Video::CaptureMode::Snapshot;
        options.audio = audio;

        started = recorder.start(host, path, options);
        startTime = now;

        if (!started)
        {
            LOG("recording start FAILED -- see the reason above");
            Apps::quit();
            return;
        }

        LOG(std::string(screen ? "screen" : "snapshot") + " capture + audio -> "
            + path);
    }

    void stopRecording()
    {
        stopping = true;

        recorder.stop().then(
            [this]
            {
                LOG("recording finished: " + path + " (dropped "
                    + std::to_string(recorder.droppedAudioFrames())
                    + " audio frames)");
                Apps::quit();
            });
    }

    Host host;
    Clock clock;
    Video::AudioSpec audio;
    Video::VideoRecorder recorder;
    ToneDevice device {recorder, audio, clock};

    WindowOptions options = []
    {
        auto o = WindowOptions();
        o.width = 320;
        o.height = 200;
        return o;
    }();
    Window window {options};

    bool started = false;
    bool stopping = false;
    double startTime = 0.0;
    std::string path;

    Threads::DisplayLink link {[this] { tick(); }};
};

int main()
{
    return eacp::Apps::run<App>();
}
