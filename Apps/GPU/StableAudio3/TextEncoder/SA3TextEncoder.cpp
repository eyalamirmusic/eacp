#include "SA3TextEncoder.h"

#include <eacp/GPU/Codegen/ComputeProgram.h>
#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Loader/Json.h>

#include <cstdint>
#include <cstring>
#include <fstream>

namespace eacp::SA3TextEncoder
{
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
class PadRowSelectKernel final : public ComputeProgram
{
public:
    PadRowSelectKernel()
    {
        compile();
    }

    void dispatch(ComputePass& pass, int rows, int dim, int validRowCount)
    {
        dimension = (std::uint32_t) dim;
        validRows = (std::uint32_t) validRowCount;
        pass.dispatch(*this, rows * dim);
    }

    Uniform<InputBuffer> encoded;
    Uniform<InputBuffer> paddingEmbedding;
    Uniform<OutputBuffer> output;
    Uniform<UInt> dimension;
    Uniform<UInt> validRows;

    EACP_SHADER(encoded, paddingEmbedding, output, dimension, validRows)

private:
    void define() override
    {
        auto i = threadId();
        auto col = i % dimension;
        auto row = i / dimension;

        write(output, i, select(row < validRows, encoded[i], paddingEmbedding[col]));
    }
};

std::vector<float> readSingleF32TensorFromSafetensors(const std::string& path,
                                                       const std::string& tensorName,
                                                       int elementCount)
{
    auto file = std::ifstream {path, std::ios::binary};

    auto headerLength = std::uint64_t {};
    file.read(reinterpret_cast<char*>(&headerLength), sizeof(headerLength));

    auto headerText = std::string((std::size_t) headerLength, '\0');
    file.read(headerText.data(), (std::streamsize) headerLength);

    auto parsed = Json::parse(headerText);
    auto entry = parsed->find(tensorName);

    const auto& offsets = entry->find("data_offsets")->asArray();
    auto startOffset = (std::uint64_t) offsets[0].asNumber();

    auto dataStart = std::uint64_t {8} + headerLength;
    auto values = std::vector<float>((std::size_t) elementCount);

    file.seekg((std::streamoff) (dataStart + startOffset));
    file.read(reinterpret_cast<char*>(values.data()),
             (std::streamsize) (values.size() * sizeof(float)));

    return values;
}
}

SA3TextEncoderModel::SA3TextEncoderModel(BpeTokenizer tokenizerToUse,
                                        T5GemmaEncoder encoderToUse,
                                        Tensor paddingEmbeddingToUse)
    : tokenizer(std::move(tokenizerToUse))
    , encoder(std::move(encoderToUse))
    , paddingEmbedding(std::move(paddingEmbeddingToUse))
{
}

std::optional<SA3TextEncoderModel> SA3TextEncoderModel::load(
    const std::string& tokenizerJsonPath,
    const std::string& t5gemmaSafetensorsPath,
    const std::string& conditionerSafetensorsPath,
    Device& device)
{
    auto tokenizer = BpeTokenizer::load(tokenizerJsonPath);

    if (!tokenizer.has_value())
        return std::nullopt;

    auto encoder = T5GemmaEncoder::load(t5gemmaSafetensorsPath, device);

    if (!encoder.has_value())
        return std::nullopt;

    auto paddingEmbeddingHost = readSingleF32TensorFromSafetensors(
        conditionerSafetensorsPath,
        "conditioner.conditioners.prompt.padding_embedding",
        T5GemmaEncoder::hiddenSize);

    auto paddingEmbedding =
        Tensor::fromHostF32(paddingEmbeddingHost.data(), {T5GemmaEncoder::hiddenSize}, device);

    return SA3TextEncoderModel {
        std::move(*tokenizer), std::move(*encoder), std::move(paddingEmbedding)};
}

PromptEncoding SA3TextEncoderModel::encodePrompt(ComputePass& pass,
                                                const std::string& text,
                                                Device& device) const
{
    auto tokenized = tokenizer.encode(text, maxLength);
    auto encoded = encoder.encodeTokens(pass, tokenized.ids, tokenized.validLength, device);

    auto hiddenSize = T5GemmaEncoder::hiddenSize;
    auto result = Tensor::uninitializedF32({maxLength, hiddenSize}, device);

    auto& kernel = GPU::sharedKernel<PadRowSelectKernel>(device);
    kernel.encoded = encoded.buffer();
    kernel.paddingEmbedding = paddingEmbedding.buffer();
    kernel.output = result.buffer();
    kernel.dispatch(pass, maxLength, hiddenSize, tokenized.validLength);

    return PromptEncoding {std::move(result), tokenized.validLength};
}
}
