#include "Common.h"

#include <array>
#include <atomic>
#include <string>
#include <vector>

using namespace nano;
using namespace eacp;
using namespace eacp::Video;
using VideoTests::clip;
using VideoTests::pumpFor;
using VideoTests::waitFor;

// The formats here are the platform decoder's own output, so these tests pin
// the whole native pipeline: container parsing, decode, colour conversion and
// the frame hand-off the render path pulls from.

auto tOpenReady = test("Video/openReachesReadyWithMetadata") = []
{
    auto player = Player {};
    auto readyCalls = 0;
    player.onReady = [&readyCalls] { ++readyCalls; };

    check(player.open(clip("bunny720.mp4")));
    check(waitFor([&] { return player.state() == PlayerState::Ready; }));
    check(waitFor([&] { return readyCalls == 1; }));

    check(player.width() == 1280);
    check(player.height() == 720);
    check(player.duration() > 9.0);
    check(player.duration() < 11.0);
};

auto tMissingFile = test("Video/missingFileReportsFailure") = []
{
    auto player = Player {};
    auto errors = std::vector<std::string> {};
    player.onError = [&errors](const std::string& message)
    { errors.push_back(message); };

    player.open(FilePath {"media/does-not-exist.mp4"});

    check(waitFor([&] { return player.state() == PlayerState::Failed; }));
    check(waitFor([&] { return !errors.empty(); }));
};

auto tFramesArrive = test("Video/playbackDeliversFrames") = []
{
    auto player = Player {};
    check(player.open(clip("jellyfish.mp4")));
    check(waitFor([&] { return player.state() == PlayerState::Ready; }));

    player.play();
    check(waitFor([&] { return player.frameSequence() >= 3; }));

    // Paused and settled so no new frame can land between the two copies
    // gated against each other at the end.
    player.pause();
    pumpFor(150);

    auto frame = FramePixels {};
    check(player.copyLatestFrame(frame));
    check(frame.width == player.width());
    check(frame.height == player.height());
    check((int) frame.data.size() == frame.width * frame.height * 4);
    check(frame.sequence > 0);

    // Tightly packed BGRA with the alpha forced opaque — what the blended
    // sprite pipeline requires of the upload path.
    auto opaque = true;

    for (auto y: {0, frame.height / 2, frame.height - 1})
        for (auto x: {0, frame.width / 2, frame.width - 1})
        {
            auto index =
                ((std::size_t) y * (std::size_t) frame.width + (std::size_t) x) * 4
                + 3;
            opaque = opaque && frame.data[index] == 0xff;
        }

    check(opaque);

    // The same sequence is never handed out twice.
    check(!player.copyLatestFrame(frame));
};

auto tClockAdvances = test("Video/playbackAdvancesClock") = []
{
    auto player = Player {};
    check(player.open(clip("bunny720.mp4")));
    check(waitFor([&] { return player.state() == PlayerState::Ready; }));

    player.play();
    check(player.isPlaying());
    check(waitFor([&] { return player.currentTime() > 0.3; }));
};

auto tPauseHolds = test("Video/pauseHoldsClockAndFrames") = []
{
    auto player = Player {};
    check(player.open(clip("bunny720.mp4")));
    check(waitFor([&] { return player.state() == PlayerState::Ready; }));

    player.play();
    check(waitFor([&] { return player.currentTime() > 0.2; }));

    player.pause();
    check(!player.isPlaying());
    pumpFor(100); // let an in-flight frame settle

    auto time = player.currentTime();
    auto sequence = player.frameSequence();
    pumpFor(400);

    check(player.currentTime() == time);
    check(player.frameSequence() == sequence);
};

auto tSeek = test("Video/seekJumpsTheClock") = []
{
    auto player = Player {};
    check(player.open(clip("bunny720.mp4")));
    check(waitFor([&] { return player.state() == PlayerState::Ready; }));

    player.play();
    check(waitFor([&] { return player.currentTime() > 0.2; }));

    auto target = player.duration() - 3.0;
    player.seek(target);

    check(waitFor([&] { return player.currentTime() >= target - 1.0; }));
    check(player.currentTime() <= player.duration() + 0.5);
};

auto tLoopWraps = test("Video/loopWrapsToTheStart") = []
{
    auto player = Player {};
    auto ended = 0;
    player.onEnded = [&ended] { ++ended; };

    check(player.open(clip("sintel.mp4")));
    check(waitFor([&] { return player.state() == PlayerState::Ready; }));

    player.setLooping(true);
    check(player.isLooping());
    player.play();

    player.seek(player.duration() - 0.5);

    check(waitFor([&] { return ended > 0; }));
    check(waitFor([&] { return player.currentTime() < 2.0; }));
    check(player.isPlaying());
};

auto tEndStops = test("Video/endWithoutLoopStops") = []
{
    auto player = Player {};
    auto ended = 0;
    player.onEnded = [&ended] { ++ended; };

    check(player.open(clip("jellyfish.mp4")));
    check(waitFor([&] { return player.state() == PlayerState::Ready; }));

    player.play();
    player.seek(player.duration() - 0.5);

    check(waitFor([&] { return ended > 0 && !player.isPlaying(); }));
};

auto tCloseReopen = test("Video/closeResetsAndReopens") = []
{
    auto player = Player {};
    check(player.open(clip("bunny720.mp4")));
    check(waitFor([&] { return player.state() == PlayerState::Ready; }));

    player.play();
    check(waitFor([&] { return player.frameSequence() >= 1; }));

    player.close();
    check(player.state() == PlayerState::Idle);
    check(player.frameSequence() == 0);
    check(player.width() == 0);
    check(player.height() == 0);

    check(player.open(clip("bunny720.mp4")));
    check(waitFor([&] { return player.state() == PlayerState::Ready; }));

    player.play();
    check(waitFor([&] { return player.frameSequence() >= 1; }));
};

auto tConcurrent = test("Video/fourClipsDecodeSimultaneously") = []
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

    check(players[0].width() == 1920);
    check(players[0].height() == 1080);

    for (auto& player: players)
        player.play();

    // Every clip keeps decoding and advancing at once — live tiles, not
    // frozen thumbnails.
    check(waitFor(
        [&]
        {
            for (auto& player: players)
                if (player.frameSequence() < 5 || player.currentTime() < 0.3)
                    return false;

            return true;
        },
        30000));
};

// The push half of the display path: the decode thread announces each frame,
// which is what lets a view redraw at the clip's rate instead of the display's.
auto tFrameArrived = test("Video/frameArrivedAnnouncesEveryNewFrame") = []
{
    auto player = Player {};
    std::atomic<int> arrivals {0};

    check(player.open(clip("bunny720.mp4")));
    check(waitFor([&] { return player.state() == PlayerState::Ready; }));

    player.setFrameArrivedCallback([&arrivals] { ++arrivals; });
    player.play();

    check(waitFor([&] { return arrivals.load() >= 5; }));

    // One announcement per published frame, not per pull: the two counters
    // track each other.
    auto announced = arrivals.load();
    check(announced <= (int) player.frameSequence());

    // Clearing it stops delivery, and nothing keeps firing into a dead
    // consumer — the case a detaching view relies on.
    player.setFrameArrivedCallback({});
    auto settled = arrivals.load();
    pumpFor(300);
    check(arrivals.load() == settled);

    // A paused player announces nothing at all.
    player.setFrameArrivedCallback([&arrivals] { ++arrivals; });
    player.pause();
    pumpFor(150);
    auto paused = arrivals.load();
    pumpFor(300);
    check(arrivals.load() == paused);
};

// The hover-switching stress the demo used to assert: whichever clip is on
// stage changes constantly, and none of them may stall while it happens.
auto tRapidSwitching = test("Video/rapidSwitchingNeverStallsAClip") = []
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

    check(waitFor([&] { return players[0].frameSequence() >= 2; }));

    auto before = std::array<std::uint64_t, 4> {};

    for (auto index = std::size_t {0}; index < players.size(); ++index)
        before[index] = players[index].frameSequence();

    // 24 switches at 60ms, the demo's old sweep. Switching only changes which
    // player is drawn, so it must not perturb decoding at all.
    for (auto switches = 0; switches < 24; ++switches)
    {
        auto active = (std::size_t) (switches % 4);
        auto frame = FramePixels {};
        players[active].copyLatestFrame(frame);
        pumpFor(60);
    }

    // Every clip advanced across the sweep, including the three that were
    // never the active one.
    for (auto index = std::size_t {0}; index < players.size(); ++index)
        check(players[index].frameSequence() > before[index]);
};
