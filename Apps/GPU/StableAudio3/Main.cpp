#include "Checkpoints.h"

#include <eacp/Core/Utils/FilePath.h>
#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Loader/SafetensorsFile.h>
#include <Codec/SA3Codec.h>
#include <Codec/WavFile.h>
#include <DiT/SA3DiT.h>
#include <DiT/Weights.h>
#include <Sampler/Sampler.h>
#include <TextEncoder/SA3TextEncoder.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
constexpr auto sampleRate = 44100;
constexpr auto downsamplingRatio = 4096;
constexpr auto samplerSteps = 8;

struct Options
{
    std::string prompt = "lofi house loop";
    float seconds = 8.f;
    std::string output = "stable-audio-3-output.wav";
    std::uint64_t seed = 42;
    std::string model = "small";
};

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start)
{
    return std::chrono::duration<double> {Clock::now() - start}.count();
}

Options parseOptions(int argc, char** argv)
{
    auto options = Options {};

    for (auto i = 1; i < argc; ++i)
    {
        auto flag = std::string {argv[i]};
        auto hasValue = i + 1 < argc;

        if (flag == "--prompt" && hasValue)
            options.prompt = argv[++i];
        else if (flag == "--seconds" && hasValue)
            options.seconds = std::stof(argv[++i]);
        else if (flag == "--output" && hasValue)
            options.output = argv[++i];
        else if (flag == "--seed" && hasValue)
            options.seed = (std::uint64_t) std::stoull(argv[++i]);
        else if (flag == "--model" && hasValue)
            options.model = argv[++i];
    }

    return options;
}

int latentLengthFor(int sampleCount)
{
    return (sampleCount + downsamplingRatio - 1) / downsamplingRatio;
}
} // namespace

int main(int argc, char** argv)
{
    auto options = parseOptions(argc, argv);
    auto totalStart = Clock::now();

    if (options.model != "small" && options.model != "medium")
    {
        std::fprintf(stderr, "--model must be small or medium.\n");
        return 1;
    }

    auto isMedium = options.model == "medium";
    auto& repo = isMedium ? SA3Checkpoints::medium : SA3Checkpoints::smallMusic;
    auto modelFile = FilePath {};
    auto tokenizerFile = FilePath {};
    auto textEncoderFile = FilePath {};

    try
    {
        modelFile = SA3Checkpoints::fetch(repo, "model.safetensors");
        tokenizerFile =
            SA3Checkpoints::fetch(repo, "t5gemma-b-b-ul2/tokenizer.json");
        textEncoderFile =
            SA3Checkpoints::fetch(repo, "t5gemma-b-b-ul2/model.safetensors");
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }

    auto& device = Device::shared();

    if (!device.isValid())
    {
        std::fprintf(stderr, "No GPU device available.\n");
        return 1;
    }

    auto ditConfig =
        isMedium ? SA3DiT::DiTConfig::medium() : SA3DiT::DiTConfig::smallMusic();
    auto codecConfig =
        isMedium ? SA3Codec::CodecConfig::sameL() : SA3Codec::CodecConfig::sameS();

    auto kernelWarmup = GPU::KernelWarmup {};
    SA3TextEncoder::SA3TextEncoderModel::addWarmupKernels(kernelWarmup);
    SA3DiT::addWarmupKernels(kernelWarmup);
    SA3Sampler::addWarmupKernels(kernelWarmup);
    SA3Codec::addWarmupKernels(kernelWarmup);
    kernelWarmup.start(device);

    auto modelPath = modelFile.str();
    auto weights = std::optional<SA3DiT::Weights> {};
    auto decoder = std::optional<SA3Codec::SameDecoder> {};
    auto start = Clock::now();

    {
        auto file = SafetensorsFile::open(modelFile);

        if (!file.has_value())
        {
            std::fprintf(
                stderr, "Could not open checkpoint at %s\n", modelPath.c_str());
            return 1;
        }

        std::printf("Loading DiT weights (%s)...\n", options.model.c_str());
        start = Clock::now();
        weights = SA3DiT::loadWeights(*file, ditConfig, device);
        std::printf("DiT weights took %.2fs\n", secondsSince(start));

        std::printf("Loading SAME decoder...\n");
        start = Clock::now();
        decoder = SA3Codec::SameDecoder::loadFromSafetensors(
            *file, codecConfig, "pretransform.model", device);
        std::printf("Decoder took %.2fs\n", secondsSince(start));
    }

    std::printf("Loading T5Gemma text encoder...\n");
    start = Clock::now();
    auto textEncoder = SA3TextEncoder::SA3TextEncoderModel::load(
        tokenizerFile.str(), textEncoderFile.str(), modelPath, device);

    if (!textEncoder.has_value())
    {
        std::fprintf(stderr, "Could not load the text encoder.\n");
        return 1;
    }

    std::printf("Text encoder took %.2fs\n", secondsSince(start));

    start = Clock::now();
    kernelWarmup.wait();
    std::printf("Waiting for kernels took %.2fs\n", secondsSince(start));

    std::printf("Encoding prompt: \"%s\"\n", options.prompt.c_str());
    start = Clock::now();

    auto promptCommands = device.makeCommandBuffer();
    auto promptEncoding = std::optional<SA3TextEncoder::PromptEncoding> {};

    {
        auto pass = promptCommands.beginCompute();
        promptEncoding = textEncoder->encodePrompt(pass, options.prompt, device);
    }

    promptCommands.commit();
    textEncoder.reset();
    std::printf("Prompt encoding took %.2fs\n", secondsSince(start));

    auto sampleCount = (int) std::lround((double) options.seconds * sampleRate);
    auto latentLength = latentLengthFor(sampleCount);

    std::printf("Sampling %d steps over %d latent frames (%.2fs)...\n",
                samplerSteps,
                latentLength,
                options.seconds);

    start = Clock::now();

    auto latent =
        SA3Sampler::pingpongSample(*weights,
                                   promptEncoding->embeddings,
                                   latentLength,
                                   options.seconds,
                                   samplerSteps,
                                   SA3Sampler::randomNoiseSource(options.seed),
                                   device);

    std::printf("Sampling took %.2fs\n", secondsSince(start));

    weights.reset();
    promptEncoding.reset();

    std::printf("Decoding audio...\n");
    start = Clock::now();
    auto waveform = decoder->decode(latent, sampleCount, device);
    std::printf("Decoding took %.2fs\n", secondsSince(start));

    start = Clock::now();

    if (!SA3Codec::writeWavFile(options.output, waveform, sampleRate))
    {
        std::fprintf(stderr, "Could not write %s\n", options.output.c_str());
        return 1;
    }

    std::printf("WAV write took %.2fs\n", secondsSince(start));
    std::printf("Wrote %s\n", options.output.c_str());
    std::printf("Total took %.2fs\n", secondsSince(totalStart));
    return 0;
}
