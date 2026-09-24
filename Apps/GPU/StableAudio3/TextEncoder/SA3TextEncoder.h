#pragma once

#include "Encoder/T5GemmaEncoder.h"
#include "Tokenizer/BpeTokenizer.h"

#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Tensor/Tensor.h>

#include <optional>
#include <string>

namespace eacp::SA3TextEncoder
{
struct PromptEncoding
{
    ML::Tensor embeddings;
    int validLength = 0;
};

class SA3TextEncoderModel
{
public:
    static std::optional<SA3TextEncoderModel> load(
        const std::string& tokenizerJsonPath,
        const std::string& t5gemmaSafetensorsPath,
        const std::string& conditionerSafetensorsPath,
        GPU::Device& device = GPU::Device::shared());

    PromptEncoding encodePrompt(GPU::ComputePass& pass,
                                const std::string& text,
                                GPU::Device& device = GPU::Device::shared()) const;

    static constexpr int maxLength = 256;

private:
    SA3TextEncoderModel(BpeTokenizer tokenizerToUse,
                       T5GemmaEncoder encoderToUse,
                       ML::Tensor paddingEmbeddingToUse);

    BpeTokenizer tokenizer;
    T5GemmaEncoder encoder;
    ML::Tensor paddingEmbedding;
};
}
