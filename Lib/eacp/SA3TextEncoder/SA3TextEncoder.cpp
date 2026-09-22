#include "SA3TextEncoder.h"

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

    auto host = encoded.toHostF32();
    auto padding = paddingEmbedding.toHostF32();
    auto hiddenSize = T5GemmaEncoder::hiddenSize;

    for (auto row = tokenized.validLength; row < maxLength; ++row)
        std::copy(padding.begin(),
                 padding.end(),
                 host.begin() + (std::ptrdiff_t) row * hiddenSize);

    auto result = Tensor::fromHostF32(host.data(), {maxLength, hiddenSize}, device);

    return PromptEncoding {std::move(result), tokenized.validLength};
}
}
