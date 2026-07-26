#include "Common.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace nano;
using namespace eacp;
using namespace eacp::Video;
using VideoTests::clip;
using VideoTests::waitFor;

// Perf gates for the decode pipeline. Thresholds are deliberately loose —
// they exist to catch order-of-magnitude regressions (a serialised pipeline,
// a lock held across a frame conversion, scheduler-quantised pacing), not to
// benchmark the machine; the measured numbers are printed so a human can see
// the real margins.

namespace
{
double seconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

auto tThroughput = test("VideoPerf/decodeOutpacesRealtimeAt8x") = []
{
    auto player = Player {};
    check(player.open(clip("bunny720.mp4")));
    check(waitFor([&] { return player.state() == PlayerState::Ready; }));

    player.setLooping(true);
    player.setRate(8.0);
    player.play();
    check(waitFor([&] { return player.frameSequence() >= 1; }));

    auto start = seconds();
    auto startSequence = player.frameSequence();
    VideoTests::pumpFor(2000);
    auto elapsed = seconds() - start;
    auto frames = (double) (player.frameSequence() - startSequence);
    auto fps = frames / elapsed;

    std::printf(
        "[perf] 720p decode at rate 8: %.1f frames/s over %.2f s\n", fps, elapsed);

    // ~30 fps content at rate 8 asks for 240; well under that still proves
    // decode + conversion + hand-off run far ahead of realtime.
    check(fps >= 80.0);
};

auto tFramePullLatency = test("VideoPerf/framePullsStayCheapUnderLoad") = []
{
    const std::array<const char*, 4> names {
        "heavy.mp4", "jellyfish.mp4", "sintel.mp4", "bunny720.mp4"};

    auto players = std::array<Player, 4> {};

    for (auto index = std::size_t {0}; index < players.size(); ++index)
    {
        players[index].setLooping(true);
        check(players[index].open(clip(names[index])));
    }

    check(waitFor(
        [&]
        {
            for (auto& player: players)
                if (player.state() != PlayerState::Ready)
                    return false;

            return true;
        }));

    for (auto& player: players)
        player.play();

    check(waitFor(
        [&]
        {
            for (auto& player: players)
                if (player.frameSequence() < 2)
                    return false;

            return true;
        }));

    // The render thread's exact access pattern: poll the sequence, then pull
    // the frame, against all four decoders running. Every critical section on
    // the frame hand-off is O(1), so no pull should ever wait behind a
    // frame's worth of conversion. Gated on p95 rather than the worst sample:
    // the absolute worst is the scheduler's to ruin (ctest runs suites in
    // parallel), while a contended lock would drag the whole distribution.
    auto scratch = std::array<FramePixels, 4> {};
    auto polls = std::vector<double> {};
    auto copies = std::vector<double> {};

    for (auto iteration = 0; iteration < 300; ++iteration)
    {
        for (auto index = std::size_t {0}; index < players.size(); ++index)
        {
            auto t0 = seconds();
            players[index].frameSequence();
            auto t1 = seconds();
            auto copied = players[index].copyLatestFrame(scratch[index]);
            auto t2 = seconds();

            polls.push_back(t1 - t0);

            if (copied)
                copies.push_back(t2 - t1);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    check(!copies.empty());

    std::sort(polls.begin(), polls.end());
    std::sort(copies.begin(), copies.end());

    auto p95 = [](const std::vector<double>& sorted)
    { return sorted[(std::size_t) ((double) sorted.size() * 0.95)]; };

    std::printf("[perf] frame pulls under 4-clip load: poll p95 %.3f ms / "
                "worst %.3f ms, copy p95 %.2f ms / worst %.2f ms over %zu "
                "copies\n",
                p95(polls) * 1000.0,
                polls.back() * 1000.0,
                p95(copies) * 1000.0,
                copies.back() * 1000.0,
                copies.size());

    check(p95(polls) < 0.001);
    check(p95(copies) < 0.015);
};

auto tPacing = test("VideoPerf/pacingHoldsSteadyCadence") = []
{
    auto player = Player {};
    check(player.open(clip("bunny720.mp4")));
    check(waitFor([&] { return player.state() == PlayerState::Ready; }));

    player.setLooping(true);
    player.play();
    check(waitFor([&] { return player.frameSequence() >= 2; }));

    // Spin-poll so the sampling itself adds no scheduler quantisation; the
    // intervals between sequence bumps are the delivered frame cadence.
    auto intervals = std::vector<double> {};
    auto lastSequence = player.frameSequence();
    auto lastBump = seconds();
    auto deadline = lastBump + 2.0;

    while (seconds() < deadline)
    {
        auto sequence = player.frameSequence();

        if (sequence != lastSequence)
        {
            auto time = seconds();
            intervals.push_back(time - lastBump);
            lastBump = time;
            lastSequence = sequence;
        }

        std::this_thread::yield();
    }

    check(intervals.size() >= 20);

    std::sort(intervals.begin(), intervals.end());
    auto median = intervals[intervals.size() / 2];
    auto p95 = intervals[(std::size_t) ((double) intervals.size() * 0.95)];
    auto worst = intervals.back();

    std::printf("[perf] frame cadence: median %.1f ms, p95 %.1f ms, "
                "worst %.1f ms over %zu frames\n",
                median * 1000.0,
                p95 * 1000.0,
                worst * 1000.0,
                intervals.size());

    // The clip plays at its native frame rate (24-60 fps), and no frame
    // should arrive whole refreshes late.
    check(median > 0.010);
    check(median < 0.060);
    check(p95 < 3.0 * median);
};
