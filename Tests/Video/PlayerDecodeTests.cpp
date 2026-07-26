#include "Common.h"

#include <array>
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

    auto frame = PlayerFramePixels {};
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
