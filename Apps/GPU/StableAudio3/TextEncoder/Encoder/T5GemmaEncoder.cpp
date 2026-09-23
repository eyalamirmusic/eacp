#include "T5GemmaEncoder.h"

#include "GemmaAttention.h"

#include <eacp/GPU/Codegen/ComputeProgram.h>
#include <eacp/GPU/Codegen/PackedVertex.h>
#include <eacp/ML/Kernels/Activation.h>
#include <eacp/ML/Kernels/Linear.h>
#include <eacp/ML/Kernels/Norm.h>
#include <eacp/ML/Kernels/RoPE.h>

#include <cmath>
#include <cstring>

namespace eacp::SA3TextEncoder
{
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
constexpr auto rmsEpsilon = 1e-6f;
constexpr auto ropeTheta = 10000.f;
constexpr auto attentionScale = 0.125f;
constexpr auto attentionSoftcap = 50.f;
constexpr auto maskedScore = -1.0e9f;

class BinaryOpKernel final : public ComputeProgram
{
public:
    enum class Op
    {
        Add,
        Multiply
    };

    explicit BinaryOpKernel(Op opToUse)
        : op(opToUse)
    {
        compile();
    }

    void dispatch(ComputePass& pass, int count)
    {
        pass.dispatch(*this, count);
    }

    Uniform<InputBuffer> left;
    Uniform<InputBuffer> right;
    Uniform<OutputBuffer> output;

    EACP_SHADER(left, right, output)

private:
    void define() override
    {
        auto i = threadId();

        if (op == Op::Add)
            write(output, i, left[i] + right[i]);
        else
            write(output, i, left[i] * right[i]);
    }

    Op op;
};

Tensor addElementwise(ComputePass& pass, const Tensor& left, const Tensor& right, Device& device)
{
    auto result = Tensor::uninitializedF32(left.shape(), device);

    auto kernel = BinaryOpKernel {BinaryOpKernel::Op::Add};
    kernel.left = left.buffer();
    kernel.right = right.buffer();
    kernel.output = result.buffer();
    kernel.prepare(device);
    kernel.dispatch(pass, left.count());

    return result;
}

Tensor multiplyElementwise(ComputePass& pass,
                          const Tensor& left,
                          const Tensor& right,
                          Device& device)
{
    auto result = Tensor::uninitializedF32(left.shape(), device);

    auto kernel = BinaryOpKernel {BinaryOpKernel::Op::Multiply};
    kernel.left = left.buffer();
    kernel.right = right.buffer();
    kernel.output = result.buffer();
    kernel.prepare(device);
    kernel.dispatch(pass, left.count());

    return result;
}

Tensor flattenHeads(Tensor&& tensor, int rows, int hiddenSize)
{
    return Tensor {std::move(tensor.buffer()), {rows, hiddenSize}, tensor.dtype()};
}

std::vector<float> readAsF32(const SafetensorsFile& file, const std::string& name)
{
    auto entry = file.find(name);
    auto count = elementCountOf(entry->shape);
    auto bytes = file.rawBytes(name);
    auto values = std::vector<float>((std::size_t) count);

    if (entry->dtype == SafetensorsDType::F32)
    {
        std::memcpy(values.data(), bytes, (std::size_t) count * sizeof(float));
        return values;
    }

    for (auto i = 0; i < count; ++i)
    {
        auto bits = std::uint16_t {};
        std::memcpy(&bits, bytes + i * 2, sizeof(bits));

        values[(std::size_t) i] = entry->dtype == SafetensorsDType::BF16
                                     ? bfloat16ToFloat(bits)
                                     : halfToFloat(bits);
    }

    return values;
}

Tensor loadAsF32(const SafetensorsFile& file, const std::string& name, Device& device)
{
    auto entry = file.find(name);
    auto values = readAsF32(file, name);

    return Tensor::fromHostF32(values.data(), entry->shape, device);
}

Tensor loadGammaPlusOne(const SafetensorsFile& file, const std::string& name, Device& device)
{
    auto entry = file.find(name);
    auto values = readAsF32(file, name);

    for (auto& value: values)
        value += 1.f;

    return Tensor::fromHostF32(values.data(), entry->shape, device);
}

Tensor buildInvFreq(int headDimension, float theta, Device& device)
{
    auto half = headDimension / 2;
    auto values = std::vector<float>((std::size_t) half);

    for (auto i = 0; i < half; ++i)
        values[(std::size_t) i] =
            1.f / std::pow(theta, (float) (2 * i) / (float) headDimension);

    return Tensor::fromHostF32(values.data(), {half}, device);
}

Tensor gatherEmbeddings(const SafetensorsFile& file,
                       const std::vector<int>& tokenIds,
                       int hiddenSize,
                       Device& device)
{
    constexpr auto embedName = "model.encoder.embed_tokens.weight";
    auto entry = file.find(embedName);
    auto raw = file.rawBytes(embedName);
    auto elementBytes = entry->dtype == SafetensorsDType::F32 ? 4 : 2;

    auto values =
        std::vector<float>((std::size_t) tokenIds.size() * (std::size_t) hiddenSize);

    for (auto i = std::size_t {0}; i < tokenIds.size(); ++i)
    {
        auto rowBytes = raw
                      + (std::size_t) tokenIds[i] * (std::size_t) hiddenSize
                          * (std::size_t) elementBytes;
        auto rowOut = values.data() + i * (std::size_t) hiddenSize;

        if (entry->dtype == SafetensorsDType::F32)
        {
            std::memcpy(rowOut, rowBytes, (std::size_t) hiddenSize * sizeof(float));
            continue;
        }

        for (auto d = 0; d < hiddenSize; ++d)
        {
            auto bits = std::uint16_t {};
            std::memcpy(&bits, rowBytes + d * 2, sizeof(bits));

            rowOut[d] = entry->dtype == SafetensorsDType::BF16 ? bfloat16ToFloat(bits)
                                                               : halfToFloat(bits);
        }
    }

    auto normalizer = std::sqrt((float) hiddenSize);

    for (auto& value: values)
        value *= normalizer;

    return Tensor::fromHostF32(values.data(), {(int) tokenIds.size(), hiddenSize}, device);
}

Tensor buildPaddingMask(int rows, int cols, int validLength, Device& device)
{
    auto values = std::vector<float>((std::size_t) rows * (std::size_t) cols, 0.f);

    for (auto row = 0; row < rows; ++row)
        for (auto col = validLength; col < cols; ++col)
            values[(std::size_t) row * (std::size_t) cols + (std::size_t) col] = maskedScore;

    return Tensor::fromHostF32(values.data(), {rows, cols}, device);
}
}

T5GemmaEncoder::T5GemmaEncoder(SafetensorsFile fileToUse,
                              std::vector<Layer> layersToUse,
                              Tensor finalNormGammaToUse,
                              Tensor invFreqToUse)
    : file(std::move(fileToUse))
    , layers(std::move(layersToUse))
    , finalNormGamma(std::move(finalNormGammaToUse))
    , invFreq(std::move(invFreqToUse))
{
}

std::optional<T5GemmaEncoder> T5GemmaEncoder::load(const std::string& safetensorsPath,
                                                   Device& device)
{
    auto file = SafetensorsFile::open(FilePath {safetensorsPath});

    if (!file.has_value())
        return std::nullopt;

    auto layers = std::vector<Layer> {};
    layers.reserve((std::size_t) numLayers);

    for (auto i = 0; i < numLayers; ++i)
    {
        auto prefix = "model.encoder.layers." + std::to_string(i) + ".";

        auto layer = Layer {
            .preSelfAttnNormGamma =
                loadGammaPlusOne(*file, prefix + "pre_self_attn_layernorm.weight", device),
            .postSelfAttnNormGamma =
                loadGammaPlusOne(*file, prefix + "post_self_attn_layernorm.weight", device),
            .preFeedforwardNormGamma =
                loadGammaPlusOne(*file, prefix + "pre_feedforward_layernorm.weight", device),
            .postFeedforwardNormGamma =
                loadGammaPlusOne(*file, prefix + "post_feedforward_layernorm.weight", device),
            .qWeight = loadAsF32(*file, prefix + "self_attn.q_proj.weight", device),
            .kWeight = loadAsF32(*file, prefix + "self_attn.k_proj.weight", device),
            .vWeight = loadAsF32(*file, prefix + "self_attn.v_proj.weight", device),
            .oWeight = loadAsF32(*file, prefix + "self_attn.o_proj.weight", device),
            .gateWeight = loadAsF32(*file, prefix + "mlp.gate_proj.weight", device),
            .upWeight = loadAsF32(*file, prefix + "mlp.up_proj.weight", device),
            .downWeight = loadAsF32(*file, prefix + "mlp.down_proj.weight", device),
        };

        layers.push_back(std::move(layer));
    }

    auto finalNormGamma = loadGammaPlusOne(*file, "model.encoder.norm.weight", device);
    auto invFreq = buildInvFreq(headDim, ropeTheta, device);

    return T5GemmaEncoder {
        std::move(*file), std::move(layers), std::move(finalNormGamma), std::move(invFreq)};
}

Tensor T5GemmaEncoder::runLayers(ComputePass& pass,
                                Tensor hidden,
                                const Tensor& mask,
                                int layerCount,
                                Device& device) const
{
    for (auto i = 0; i < layerCount; ++i)
    {
        const auto& layer = layers[(std::size_t) i];
        auto rows = hidden.rows();

        auto normed1 = rmsNorm(pass, hidden, layer.preSelfAttnNormGamma, rmsEpsilon, device);

        auto q = linear(pass, normed1, layer.qWeight, nullptr, device);
        auto k = linear(pass, normed1, layer.kWeight, nullptr, device);
        auto v = linear(pass, normed1, layer.vWeight, nullptr, device);

        auto qRope = applyRoPE(pass, q, invFreq, numHeads, headDim, device);
        auto kRope = applyRoPE(pass, k, invFreq, numHeads, headDim, device);

        auto attnOut = gemmaSelfAttention(pass,
                                          qRope,
                                          kRope,
                                          v,
                                          mask,
                                          numHeads,
                                          headDim,
                                          attentionScale,
                                          attentionSoftcap,
                                          device);

        auto attnFlat = flattenHeads(std::move(attnOut), rows, hiddenSize);
        auto attnProjected = linear(pass, attnFlat, layer.oWeight, nullptr, device);
        auto attnNormed =
            rmsNorm(pass, attnProjected, layer.postSelfAttnNormGamma, rmsEpsilon, device);

        hidden = addElementwise(pass, hidden, attnNormed, device);

        auto normed2 =
            rmsNorm(pass, hidden, layer.preFeedforwardNormGamma, rmsEpsilon, device);

        auto gate = linear(pass, normed2, layer.gateWeight, nullptr, device);
        auto gateActivated = applyActivation(pass, gate, ActivationKind::GeluTanh, device);
        auto up = linear(pass, normed2, layer.upWeight, nullptr, device);
        auto gated = multiplyElementwise(pass, gateActivated, up, device);
        auto mlpOut = linear(pass, gated, layer.downWeight, nullptr, device);
        auto mlpNormed =
            rmsNorm(pass, mlpOut, layer.postFeedforwardNormGamma, rmsEpsilon, device);

        hidden = addElementwise(pass, hidden, mlpNormed, device);
    }

    return hidden;
}

Tensor T5GemmaEncoder::encodeTokensThroughLayer(ComputePass& pass,
                                               const std::vector<int>& tokenIds,
                                               int validLength,
                                               int layerCount,
                                               Device& device) const
{
    auto hidden = gatherEmbeddings(file, tokenIds, hiddenSize, device);
    auto mask =
        buildPaddingMask((int) tokenIds.size(), (int) tokenIds.size(), validLength, device);

    return runLayers(pass, std::move(hidden), mask, layerCount, device);
}

Tensor T5GemmaEncoder::encodeTokens(ComputePass& pass,
                                   const std::vector<int>& tokenIds,
                                   int validLength,
                                   Device& device) const
{
    auto hidden = encodeTokensThroughLayer(pass, tokenIds, validLength, numLayers, device);

    return rmsNorm(pass, hidden, finalNormGamma, rmsEpsilon, device);
}
}
