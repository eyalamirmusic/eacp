#pragma once

#include "Types.h"

#include <eacp/Core/Threads/Async.h>
#include <eacp/Core/Utils/Environment.h>
#include <MakeASound/MakeASound.h>
#include <Miro/Miro.h>
#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Api
{
namespace Detail
{
inline std::string lower(std::string value)
{
    std::transform(value.begin(),
                   value.end(),
                   value.begin(),
                   [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    return value;
}

inline std::string fileKind(const std::filesystem::path& path)
{
    auto ext = path.extension().string();
    if (ext.empty())
        return "file";
    return ext.substr(0, 1) == "." ? ext.substr(1) : ext;
}

inline int fuzzyScore(const std::string& name, const std::string& query)
{
    if (query.empty())
        return 1;

    auto haystack = lower(name);
    auto needle = lower(query);

    auto score = 0;
    auto searchFrom = std::size_t {0};
    auto previous = std::numeric_limits<std::size_t>::max();

    for (auto c: needle)
    {
        auto found = haystack.find(c, searchFrom);
        if (found == std::string::npos)
            return 0;

        score += 12;

        if (found == 0 || haystack[found - 1] == ' ' || haystack[found - 1] == '-'
            || haystack[found - 1] == '_' || haystack[found - 1] == '.')
        {
            score += 8;
        }

        if (previous != std::numeric_limits<std::size_t>::max()
            && found == previous + 1)
        {
            score += 18;
        }

        score -= static_cast<int>(std::min<std::size_t>(found, 24));
        previous = found;
        searchFrom = found + 1;
    }

    return score;
}

inline bool isAudioFile(const std::filesystem::path& path)
{
    auto ext = lower(path.extension().string());
    return ext == ".wav" || ext == ".mp3" || ext == ".aif" || ext == ".aiff"
           || ext == ".flac" || ext == ".m4a" || ext == ".aac" || ext == ".ogg";
}
} // namespace Detail

class TrayLauncherApi
{
public:
    ~TrayLauncherApi()
    {
        if (audioStarted)
            audioDevice.stop();
    }

    void reflect(Miro::ApiReflector& r)
    {
        using T = TrayLauncherApi;
        r.commands<&T::searchDownloads,
                   &T::armDrag,
                   &T::copyFiles,
                   &T::showCopyToast,
                   &T::playAudio,
                   &T::stopAudio,
                   &T::getPlayback,
                   &T::submitPrompt,
                   &T::dismiss>();
        r.events<&T::playback>();
    }

    SearchDownloadsResponse searchDownloads(const SearchDownloadsRequest& request)
    {
        auto response = SearchDownloadsResponse {};
        auto downloads = eacp::homeDirectory() / "Downloads";

        auto ec = std::error_code {};
        auto candidates = EA::Vector<DownloadResult> {};

        // Walk Downloads and every subfolder, flattening audio files from any
        // depth into a single result list. skip_permission_denied plus an
        // error-code increment keep an unreadable subfolder from throwing or
        // aborting the walk.
        auto options = std::filesystem::directory_options::skip_permission_denied;
        auto entries =
            std::filesystem::recursive_directory_iterator(downloads, options, ec);
        auto end = std::filesystem::recursive_directory_iterator {};

        for (; !ec && entries != end; entries.increment(ec))
        {
            const auto& entry = *entries;

            if (!entry.is_regular_file(ec))
                continue;

            const auto& path = entry.path();
            if (!Detail::isAudioFile(path))
                continue;

            auto name = path.filename().string();
            auto score = Detail::fuzzyScore(name, request.query);

            if (score == 0)
                continue;

            candidates.push_back({name,
                                  path.string(),
                                  Detail::fileKind(path),
                                  static_cast<long long>(entry.file_size(ec)),
                                  score});
        }

        std::sort(candidates.begin(),
                  candidates.end(),
                  [](const DownloadResult& a, const DownloadResult& b)
                  {
                      if (a.score != b.score)
                          return a.score > b.score;
                      return a.name < b.name;
                  });

        auto limit = std::min<size_t>(candidates.size(), 12);
        response.results.reserveAtLeast(static_cast<int>(limit));
        for (auto i = std::size_t {0}; i < limit; ++i)
            response.results.push_back(candidates[i]);

        return response;
    }

    void armDrag(const ArmDragRequest& request) const
    {
        if (onArmDrag)
            onArmDrag(request.paths);
    }

    CopyFilesResponse copyFiles(const CopyFilesRequest& request) const
    {
        return {.copied = onCopyFiles && onCopyFiles(request.paths)};
    }

    void showCopyToast(const ShowCopyToastRequest& request) const
    {
        if (onShowCopyToast)
            onShowCopyToast(request.sampleName);
    }

    void playAudio(const PlayAudioRequest& request)
    {
        ensureAudioStarted();

        auto generation = ++decodeGeneration;
        playing.store(false);
        currentPath = request.path;
        playback.publish(getPlayback());

        auto path = request.path;
        auto sampleRate = outputSampleRate;
        std::thread(
            [this, generation, path = std::move(path), sampleRate]
            {
                auto decoded = decodeAudioFile(path, sampleRate);

                eacp::Threads::callAsync(
                    [this, generation, path, decoded = std::move(decoded)]() mutable
                    {
                        if (generation != decodeGeneration.load())
                            return;

                        if (decoded.samples.empty() || decoded.channels <= 0)
                        {
                            stopAudio();
                            return;
                        }

                        {
                            auto lock = std::lock_guard {audioMutex};
                            audioSamples = std::move(decoded.samples);
                            audioChannels = decoded.channels;
                            playbackPosition.store(0);
                        }

                        currentPath = path;
                        playing.store(true);
                        playback.publish(getPlayback());
                    });
            })
            .detach();
    }

    void stopAudio()
    {
        ++decodeGeneration;
        playing.store(false);
        currentPath.clear();
        playbackPosition.store(0);
        playback.publish(getPlayback());
    }

    PlaybackState getPlayback() const
    {
        return {.playing = playing.load(), .path = currentPath};
    }

    void submitPrompt(const SubmitPromptRequest& request) const
    {
        if (onSubmit)
            onSubmit(request.text);
    }

    void dismiss() const
    {
        if (onDismiss)
            onDismiss();
    }

    std::function<void(const EA::Vector<std::string>&)> onArmDrag;
    std::function<bool(const EA::Vector<std::string>&)> onCopyFiles;
    std::function<void(const std::string&)> onShowCopyToast;
    std::function<void(const std::string&)> onSubmit;
    std::function<void()> onDismiss;
    Miro::Event<PlaybackState> playback;

private:
    void ensureAudioStarted()
    {
        if (audioStarted)
            return;

        auto config = audioDevice.getDefaultConfig();
        if (config.output)
            outputChannels = std::max(1, config.output->nChannels);
        if (config.sampleRate <= 0)
            config.sampleRate = 48000;

        audioDevice.start(config, [this](auto& info) { renderAudio(info); });
        outputSampleRate = config.sampleRate;
        outputChannels = std::max(1, config.getOutputChannels());
        audioStarted = true;
    }

    struct DecodedAudio
    {
        std::vector<float> samples;
        int channels = 0;
    };

    static DecodedAudio decodeAudioFile(const std::string& path, int sampleRate)
    {
        auto decoderConfig = ma_decoder_config_init(ma_format_f32, 0, sampleRate);
        auto decoder = ma_decoder {};

        if (ma_decoder_init_file(path.c_str(), &decoderConfig, &decoder)
            != MA_SUCCESS)
        {
            return {};
        }

        ma_uint64 frameCount = 0;
        ma_decoder_get_length_in_pcm_frames(&decoder, &frameCount);

        auto decoded = std::vector<float> {};
        decoded.resize(static_cast<std::size_t>(frameCount)
                       * decoder.outputChannels);

        ma_uint64 framesRead = 0;
        ma_decoder_read_pcm_frames(
            &decoder, decoded.data(), frameCount, &framesRead);
        decoded.resize(static_cast<std::size_t>(framesRead)
                       * decoder.outputChannels);

        auto channels = static_cast<int>(decoder.outputChannels);

        ma_decoder_uninit(&decoder);
        return {.samples = std::move(decoded), .channels = channels};
    }

    void renderAudio(MakeASound::AudioCallbackInfo& info)
    {
        auto output = info.getOutput();

        for (auto channel: output.channels())
            std::fill(channel.begin(), channel.end(), 0.0f);

        if (!playing.load(std::memory_order_relaxed))
            return;

        auto lock = std::lock_guard {audioMutex};
        if (audioSamples.empty() || audioChannels <= 0)
            return;

        auto startFrame = playbackPosition.load(std::memory_order_relaxed);
        auto totalFrames =
            audioSamples.size() / static_cast<std::size_t>(audioChannels);
        auto framesToWrite =
            std::min<std::size_t>(output.getNumSamples(), totalFrames - startFrame);

        for (auto ch = 0; ch < output.getNumChannels(); ++ch)
        {
            auto out = output.getChannel(ch);
            auto sourceChannel = std::min(ch, audioChannels - 1);

            for (auto frame = std::size_t {0}; frame < framesToWrite; ++frame)
            {
                out[static_cast<int>(frame)] =
                    audioSamples[(startFrame + frame)
                                     * static_cast<std::size_t>(audioChannels)
                                 + static_cast<std::size_t>(sourceChannel)];
            }
        }

        auto nextFrame = startFrame + framesToWrite;
        playbackPosition.store(nextFrame, std::memory_order_relaxed);

        if (nextFrame >= totalFrames)
        {
            playing.store(false, std::memory_order_relaxed);
            eacp::Threads::callAsync(
                [this]
                {
                    currentPath.clear();
                    playback.publish(getPlayback());
                });
        }
    }

    MakeASound::DeviceManager audioDevice;
    bool audioStarted = false;
    int outputChannels = 2;
    int outputSampleRate = 48000;
    std::atomic<unsigned long long> decodeGeneration {0};
    std::atomic<bool> playing {false};
    std::atomic<std::size_t> playbackPosition {0};
    std::string currentPath;
    std::mutex audioMutex;
    std::vector<float> audioSamples;
    int audioChannels = 0;
};

} // namespace Api
