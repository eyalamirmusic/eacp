#include <eacp/Core/Utils/WinInclude.h>

#include "Player.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

// Windows playback backend: a Media Foundation IMFSourceReader decoding the
// first video stream to RGB32 (BGRA in memory) on a worker thread, paced by
// sample timestamps against a steady clock. Video only for now — no audio
// stream is opened, so setMuted/setVolume are inert here. The display path is
// the CPU one: copyLatestFrame; there is no zero-copy native buffer.

namespace eacp::Video
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr double ticksPerSecond = 1e7; // MF timestamps are 100ns ticks

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
} // namespace

struct Player::Native
{
    explicit Native(Player& ownerToUse)
        : owner(ownerToUse)
    {
    }

    ~Native() { close(); }

    bool open(const FilePath& file)
    {
        close();

        quit = false;
        state = PlayerState::Loading;
        worker = std::thread([this, file] { run(file); });
        return true;
    }

    void close()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            quit = true;
        }

        wake.notify_all();

        if (worker.joinable())
            worker.join();

        *alive = false;
        alive = std::make_shared<bool>(true);

        state = PlayerState::Idle;
        playing = false;
        lastPts = 0.0;
        videoWidth = 0;
        videoHeight = 0;
        videoDuration = 0.0;

        resetFrames();
    }

    // Marshals a notification to the main thread, fenced by the alive token
    // so one queued behind close() backs off instead of dangling.
    void notify(std::function<void(Player&)> event)
    {
        Threads::callAsync(
            [this, guard = alive, event = std::move(event)]
            {
                if (*guard)
                    event(owner);
            });
    }

    void run(const FilePath& file)
    {
        // MF needs COM up on this thread; the worker is ours alone.
        auto comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto comInitialized = SUCCEEDED(comResult);

        // Sleep on Windows quantises to the ~15.6 ms scheduler tick, which
        // would push every frame up to a whole refresh late; a high-resolution
        // waitable timer paces to sub-millisecond. The plain timer is the
        // pre-1803 fallback.
        pacingTimer = CreateWaitableTimerExW(nullptr,
                                             nullptr,
                                             CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                             TIMER_ALL_ACCESS);

        if (pacingTimer == nullptr)
            pacingTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

        auto mfStarted = SUCCEEDED(MFStartup(MF_VERSION));
        auto reader = ComPtr<IMFSourceReader> {};

        if (mfStarted && configure(file, reader))
        {
            state = PlayerState::Ready;
            notify([](Player& player) { player.onReady(); });
            decodeLoop(reader);
        }
        else
        {
            state = PlayerState::Failed;
            notify([](Player& player) { player.onError("failed to open video"); });
        }

        reader.Reset();

        if (mfStarted)
            MFShutdown();

        if (pacingTimer != nullptr)
        {
            CloseHandle(pacingTimer);
            pacingTimer = nullptr;
        }

        if (comInitialized)
            CoUninitialize();
    }

    bool configure(const FilePath& file, ComPtr<IMFSourceReader>& reader)
    {
        auto url = file.wide();

        // Without a processing attribute the reader hands back the decoder's
        // own output (NV12 for H.264) and rejects the RGB32 request with
        // MF_E_INVALIDMEDIATYPE — it only inserts the video processor that can
        // convert when asked to up front. ADVANCED is the one to ask for: the
        // basic ENABLE_VIDEO_PROCESSING flag turns hardware-accelerated
        // decoding off as a side effect, ADVANCED does not.
        ComPtr<IMFAttributes> attributes;

        if (FAILED(MFCreateAttributes(&attributes, 1)))
            return false;

        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,
                              TRUE);

        if (FAILED(
                MFCreateSourceReaderFromURL(url.c_str(), attributes.Get(), &reader)))
        {
            // Pre-Win8 media stacks only know the basic flag.
            attributes->DeleteItem(
                MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING);
            attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

            if (FAILED(MFCreateSourceReaderFromURL(
                    url.c_str(), attributes.Get(), &reader)))
                return false;
        }

        reader->SetStreamSelection((DWORD) MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection((DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                   TRUE);

        ComPtr<IMFMediaType> requested;
        if (FAILED(MFCreateMediaType(&requested)))
            return false;

        requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        requested->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);

        if (FAILED(reader->SetCurrentMediaType(
                (DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                nullptr,
                requested.Get())))
            return false;

        if (!readOutputFormat(reader))
            return false;

        PROPVARIANT duration;
        PropVariantInit(&duration);

        if (SUCCEEDED(reader->GetPresentationAttribute(
                (DWORD) MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration)))
            videoDuration = (double) duration.uhVal.QuadPart / ticksPerSecond;

        PropVariantClear(&duration);
        return videoWidth > 0 && videoHeight > 0;
    }

    // The reader renegotiates its output as it starts decoding, so the geometry
    // is re-read here rather than trusted from configure() alone.
    bool readOutputFormat(ComPtr<IMFSourceReader>& reader)
    {
        ComPtr<IMFMediaType> actual;

        if (FAILED(reader->GetCurrentMediaType(
                (DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual)))
            return false;

        UINT32 w = 0, h = 0;
        MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &w, &h);
        videoWidth = (int) w;
        videoHeight = (int) h;

        // Negative means bottom-up rows; the copy below flips them.
        UINT32 strideValue = 0;
        if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideValue)))
            stride = (int) (INT32) strideValue;
        else
            stride = videoWidth * 4;

        return videoWidth > 0 && videoHeight > 0;
    }

    void decodeLoop(ComPtr<IMFSourceReader>& reader)
    {
        while (true)
        {
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock,
                          [this] { return quit || playing || pendingSeek >= 0.0; });

                if (quit)
                    return;

                if (pendingSeek >= 0.0)
                {
                    seekTo(reader, pendingSeek);
                    pendingSeek = -1.0;
                }

                if (!playing)
                    continue;
            }

            DWORD streamIndex = 0, flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;

            if (FAILED(
                    reader->ReadSample((DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                       0,
                                       &streamIndex,
                                       &flags,
                                       &timestamp,
                                       &sample)))
            {
                playing = false;
                notify([](Player& player)
                       { player.onError("video decode failed"); });
                continue;
            }

            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0)
                readOutputFormat(reader);

            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
            {
                if (looping)
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    seekTo(reader, 0.0);
                }
                else
                {
                    playing = false;
                }

                notify([](Player& player) { player.onEnded(); });
                continue;
            }

            if (!sample)
                continue;

            auto pts = (double) timestamp / ticksPerSecond;

            if (!waitUntilDue(pts))
                continue; // interrupted by pause/seek/quit

            store(sample.Get());
            lastPts = pts;
        }
    }

    // The reader restarts at the previous sync point; close enough for a
    // basic player, and exact for the loop wrap to zero.
    void seekTo(ComPtr<IMFSourceReader>& reader, double seconds)
    {
        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = (LONGLONG) (seconds * ticksPerSecond);
        reader->SetCurrentPosition(GUID_NULL, position);
        PropVariantClear(&position);

        lastPts = seconds;
        anchor = now() - seconds / rate;
    }

    // Sleeps until the sample's presentation time; false when playback state
    // changed underneath (the sample is then dropped, not shown late).
    bool waitUntilDue(double pts)
    {
        while (true)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);

                if (quit || !playing || pendingSeek >= 0.0)
                    return false;
            }

            auto due = anchor + pts / rate;
            auto remaining = due - now();

            if (remaining <= 0.0)
                return true;

            // Capped so pause/seek/quit are still noticed promptly.
            sleepFor(std::min(remaining, 0.01));
        }
    }

    void sleepFor(double seconds)
    {
        if (pacingTimer != nullptr)
        {
            auto due = LARGE_INTEGER {};
            due.QuadPart = -(LONGLONG) (seconds * ticksPerSecond);

            if (SetWaitableTimer(pacingTimer, &due, 0, nullptr, nullptr, FALSE))
            {
                WaitForSingleObject(pacingTimer, INFINITE);
                return;
            }
        }

        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    }

    // The conversion runs with no lock held: the buffer being written is
    // exclusively the decode thread's until the O(1) publish at the end, so
    // the render thread's frame pulls never wait behind a frame conversion.
    void store(IMFSample* sample)
    {
        ComPtr<IMFMediaBuffer> buffer;

        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
            return;

        BYTE* data = nullptr;
        DWORD length = 0;

        if (FAILED(buffer->Lock(&data, &length, nullptr)))
            return;

        auto next = acquireFreeBuffer();
        convertFrame(*next, data);

        buffer->Unlock();

        next->sequence = ++nextSequence;

        {
            std::lock_guard<std::mutex> lock(frameMutex);
            latest = next;
            publishedSequence.store(next->sequence, std::memory_order_release);
        }
    }

    // A small pool the decode thread recycles: a slot whose only reference is
    // the pool's own is free — readers holding a snapshot keep their slot
    // alive and it is simply skipped. Three slots cover the steady state (one
    // published, one being read, one being written); a fresh allocation
    // replacing the round-robin slot is the overload escape, not the norm.
    std::shared_ptr<PlayerFramePixels> acquireFreeBuffer()
    {
        for (auto& slot: pool)
        {
            if (slot == nullptr)
                slot = std::make_shared<PlayerFramePixels>();

            if (slot.use_count() == 1)
                return slot;
        }

        auto& slot = pool[(std::size_t) (poolCursor++ % (int) pool.size())];
        slot = std::make_shared<PlayerFramePixels>();
        return slot;
    }

    void convertFrame(PlayerFramePixels& out, const BYTE* data)
    {
        auto rowPixels = (std::size_t) videoWidth;
        auto rowBytes = rowPixels * 4;
        auto bottomUp = stride < 0;
        auto absStride = (std::size_t) (bottomUp ? -stride : stride);

        out.width = videoWidth;
        out.height = videoHeight;
        out.data.resize(rowBytes * (std::size_t) videoHeight);

        for (auto y = 0; y < videoHeight; ++y)
        {
            auto sourceRow = bottomUp ? videoHeight - 1 - y : y;
            const auto* src = reinterpret_cast<const std::uint32_t*>(
                data + (std::size_t) sourceRow * absStride);
            auto* dst = reinterpret_cast<std::uint32_t*>(
                out.data.data() + (std::size_t) y * rowBytes);

            // RGB32's X channel is undefined and the blended sprite pipeline
            // needs opaque alpha, so copy and force it in one vectorisable
            // pass instead of memcpy plus a byte-strided fix-up.
            for (std::size_t x = 0; x < rowPixels; ++x)
                dst[x] = src[x] | 0xff000000u;
        }
    }

    Player& owner;

    std::thread worker;
    HANDLE pacingTimer = nullptr; // worker thread only
    std::mutex mutex;
    std::condition_variable wake;
    bool quit = false;
    std::atomic<bool> playing {false};
    double pendingSeek = -1.0; // guarded by mutex; <0 = none

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    std::atomic<PlayerState> state {PlayerState::Idle};
    std::atomic<bool> looping {false};
    std::atomic<double> rate {1.0};
    std::atomic<double> lastPts {0.0};
    std::atomic<double> anchor {0.0};
    int videoWidth = 0;
    int videoHeight = 0;
    int stride = 0;
    double videoDuration = 0.0;

    // frameMutex guards only the `latest` pointer — every critical section on
    // it is O(1), so the render thread and four decode threads never queue
    // behind a frame's worth of work.
    mutable std::mutex frameMutex;
    std::shared_ptr<PlayerFramePixels> latest; // BGRA top-down
    std::array<std::shared_ptr<PlayerFramePixels>, 3> pool; // decode thread only
    int poolCursor = 0; // decode thread only
    std::uint64_t nextSequence = 0; // decode thread only
    std::atomic<std::uint64_t> publishedSequence {0};

    bool copyLatestFrame(PlayerFramePixels& out)
    {
        auto snap = std::shared_ptr<PlayerFramePixels> {};

        {
            std::lock_guard<std::mutex> lock(frameMutex);
            snap = latest;
        }

        if (snap == nullptr || snap->sequence == out.sequence)
            return false;

        // The copy runs outside the lock: the snapshot keeps its buffer alive
        // and the decode thread only ever writes into free pool slots.
        out.width = snap->width;
        out.height = snap->height;
        out.sequence = snap->sequence;
        out.data = snap->data;
        return true;
    }

    void resetFrames()
    {
        {
            std::lock_guard<std::mutex> lock(frameMutex);
            latest = nullptr;
            publishedSequence.store(0, std::memory_order_release);
        }

        for (auto& slot: pool)
            slot = nullptr;

        poolCursor = 0;
        nextSequence = 0;
    }
};

Player::Player()
    : impl(*this)
{
}

Player::~Player() = default;

bool Player::open(const FilePath& file)
{
    return impl->open(file);
}

void Player::close()
{
    impl->close();
}

void Player::play()
{
    impl->anchor = now() - impl->lastPts / impl->rate;

    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->playing = true;
    }

    impl->wake.notify_all();
}

void Player::pause()
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->playing = false;
}

bool Player::isPlaying() const
{
    return impl->playing;
}

void Player::setLooping(bool shouldLoop)
{
    impl->looping = shouldLoop;
}

bool Player::isLooping() const
{
    return impl->looping;
}

void Player::setMuted(bool)
{
    // No audio stream on Windows yet.
}

void Player::setVolume(float)
{
    // No audio stream on Windows yet.
}

void Player::setRate(double rate)
{
    impl->rate = rate;
    impl->anchor = now() - impl->lastPts / rate;
}

void Player::seek(double seconds)
{
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->pendingSeek = seconds;
    }

    impl->wake.notify_all();
}

PlayerState Player::state() const
{
    return impl->state;
}

int Player::width() const
{
    return impl->videoWidth;
}

int Player::height() const
{
    return impl->videoHeight;
}

double Player::duration() const
{
    return impl->videoDuration;
}

double Player::currentTime() const
{
    return impl->lastPts;
}

void* Player::acquireFramePixelBuffer()
{
    return nullptr;
}

void Player::releasePixelBuffer(void*) {}

bool Player::copyLatestFrame(PlayerFramePixels& out)
{
    return impl->copyLatestFrame(out);
}

std::uint64_t Player::frameSequence() const
{
    return impl->publishedSequence.load(std::memory_order_acquire);
}
} // namespace eacp::Video
