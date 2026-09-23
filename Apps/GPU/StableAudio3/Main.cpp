#include <eacp/Core/Utils/FilePath.h>
#include <eacp/GPU/Device/Device.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Loader/SafetensorsFile.h>
#include <eacp/SA3Codec/SA3Codec.h>
#include <eacp/SA3Codec/WavFile.h>
#include <eacp/SA3DiT/SA3DiT.h>
#include <eacp/SA3DiT/Weights.h>
#include <eacp/SA3Sampler/Sampler.h>
#include <eacp/SA3TextEncoder/SA3TextEncoder.h>

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

constexpr auto checkpointRoot =
    "/Users/jamiepond/.cache/huggingface/hub/"
    "models--stabilityai--stable-audio-3-small-music/snapshots/"
    "0fef1392cd842149a2b6d445e181c97608faac06/";

struct Options
{
    std::string prompt = "lofi house loop";
    float seconds = 8.f;
    std::string output = "stable-audio-3-output.wav";
    std::uint64_t seed = 42;
};

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
    }

    return options;
}

int latentLengthFor(int sampleCount)
{
    return (sampleCount + downsamplingRatio - 1) / downsamplingRatio;
}
}

int main(int argc, char** argv)
{
    auto options = parseOptions(argc, argv);

    auto& device = Device::shared();

    if (!device.isValid())
    {
        std::fprintf(stderr, "No GPU device available.\n");
        return 1;
    }

    auto modelPath = std::string {checkpointRoot} + "model.safetensors";
    auto file = SafetensorsFile::open(FilePath {modelPath});

    if (!file.has_value())
    {
        std::fprintf(stderr, "Could not open checkpoint at %s\n", modelPath.c_str());
        return 1;
    }

    std::printf("Loading DiT weights...\n");
    auto weights = SA3DiT::loadWeights(*file, device);

    std::printf("Loading SAME codec...\n");
    auto codec = SA3Codec::SameCodec::loadFromSafetensors(*file);

    std::printf("Loading T5Gemma text encoder...\n");
    auto textEncoder = SA3TextEncoder::SA3TextEncoderModel::load(
        std::string {checkpointRoot} + "t5gemma-b-b-ul2/tokenizer.json",
        std::string {checkpointRoot} + "t5gemma-b-b-ul2/model.safetensors",
        modelPath,
        device);

    if (!textEncoder.has_value())
    {
        std::fprintf(stderr, "Could not load the text encoder.\n");
        return 1;
    }

    std::printf("Encoding prompt: \"%s\"\n", options.prompt.c_str());

    auto promptCommands = device.makeCommandBuffer();
    auto promptEncoding = std::optional<SA3TextEncoder::PromptEncoding> {};

    {
        auto pass = promptCommands.beginCompute();
        promptEncoding = textEncoder->encodePrompt(pass, options.prompt, device);
    }

    promptCommands.commit();

    auto sampleCount = (int) std::lround((double) options.seconds * sampleRate);
    auto latentLength = latentLengthFor(sampleCount);

    std::printf("Sampling %d steps over %d latent frames (%.2fs)...\n",
               samplerSteps,
               latentLength,
               options.seconds);

    auto start = std::chrono::steady_clock::now();

    auto latent = SA3Sampler::pingpongSample(weights,
                                             promptEncoding->embeddings,
                                             latentLength,
                                             options.seconds,
                                             samplerSteps,
                                             SA3Sampler::randomNoiseSource(options.seed),
                                             device);

    auto elapsed = std::chrono::duration<double> {std::chrono::steady_clock::now() - start};
    std::printf("Sampling took %.2fs\n", elapsed.count());

    std::printf("Decoding audio...\n");
    auto waveform = codec.decode(latent, sampleCount, device);

    if (!SA3Codec::writeWavFile(options.output, waveform, sampleRate))
    {
        std::fprintf(stderr, "Could not write %s\n", options.output.c_str());
        return 1;
    }

    std::printf("Wrote %s\n", options.output.c_str());
    return 0;
}
