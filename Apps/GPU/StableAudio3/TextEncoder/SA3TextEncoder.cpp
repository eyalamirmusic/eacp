#include "SA3TextEncoder.h"

#include <eacp/GPU/Codegen/ComputeProgram.h>
#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/Frame/ComputePass.h>

#include <cstdint>

namespace eacp::SA3TextEncoder
{
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
class PadRowSelectKernel final : public ComputeProgram
{
public:
    PadRowSelectKernel() { compile(); }

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
} // namespace

SA3TextEncoderModel::SA3TextEncoderModel(BpeTokenizer tokenizerToUse,
                                         T5GemmaEncoder encoderToUse,
                                         Tensor paddingEmbeddingToUse)
    : tokenizer(std::move(tokenizerToUse))
    , encoder(std::move(encoderToUse))
    , paddingEmbedding(std::move(paddingEmbeddingToUse))
{
}

std::optional<SA3TextEncoderModel>
    SA3TextEncoderModel::load(const std::string& tokenizerJsonPath,
                              const std::string& t5gemmaSafetensorsPath,
                              const std::string& conditionerSafetensorsPath,
                              Device& device)
{
    auto conditioner = SafetensorsFile::open(FilePath {conditionerSafetensorsPath});

    if (!conditioner.has_value())
        return std::nullopt;

    return load(BpeTokenizer::load(tokenizerJsonPath),
                t5gemmaSafetensorsPath,
                *conditioner,
                device);
}

std::optional<SA3TextEncoderModel>
    SA3TextEncoderModel::load(std::optional<BpeTokenizer> tokenizer,
                              const std::string& t5gemmaSafetensorsPath,
                              const SafetensorsFile& conditioner,
                              Device& device)
{
    if (!tokenizer.has_value())
        return std::nullopt;

    auto encoder = T5GemmaEncoder::load(t5gemmaSafetensorsPath, device);

    if (!encoder.has_value())
        return std::nullopt;

    auto paddingEmbedding = conditioner.loadF32(
        "conditioner.conditioners.prompt.padding_embedding", device);

    return SA3TextEncoderModel {
        std::move(*tokenizer), std::move(*encoder), std::move(paddingEmbedding)};
}

PromptEncoding SA3TextEncoderModel::encodePrompt(ComputePass& pass,
                                                 const std::string& text,
                                                 Device& device) const
{
    auto tokenized = tokenizer.encode(text, maxLength);
    auto encoded =
        encoder.encodeTokens(pass, tokenized.ids, tokenized.validLength, device);

    auto hiddenSize = T5GemmaEncoder::hiddenSize;
    auto result = Tensor::uninitializedF32({maxLength, hiddenSize}, device);

    auto& kernel = GPU::sharedKernel<PadRowSelectKernel>(device);
    kernel.encoded = encoded;
    kernel.paddingEmbedding = paddingEmbedding;
    kernel.output = result;
    kernel.dispatch(pass, maxLength, hiddenSize, tokenized.validLength);

    return PromptEncoding {std::move(result), tokenized.validLength};
}

void forEachTextEncoderShaderGraph(const GPU::ShaderGraphVisitor& visit)
{
    visit(PadRowSelectKernel {}.graph());
}
} // namespace eacp::SA3TextEncoder
