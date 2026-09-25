#include "GraphCommon.h"
#include "WhisperDecoderStep.h"

using namespace nano;
using namespace eacp;
using namespace eacp::ML;
using namespace MLGraphTesting;

namespace
{
int countOf(const MIL::Specification& specification, std::string_view type)
{
    auto count = 0;

    for (auto& operation: specification.program.main.block.operations)
        count += operation.type == type ? 1 : 0;

    return count;
}

const MIL::ArrayFeature* featureNamed(const Vector<MIL::ArrayFeature>& features,
                                      std::string_view name)
{
    auto isNamed = [name](const MIL::ArrayFeature& feature)
    { return feature.name == name; };
    return features.findIf(isNamed);
}
} // namespace

auto tWhisperDecoderStep = test("MLGraph/DecoderStep/whisperTinyAtItsRealSizes") = []
{
    auto graph = WhisperDecoderStep::sharedStepGraph();
    auto package = buildChecked(graph);
    auto specification = graph.specification();

    check(specification.program.main.opset == "CoreML8");

    auto& inputs = specification.description.inputs;
    check(inputs.size() == 7);

    auto* selfKeys = featureNamed(inputs, "self_keys");
    auto* crossValues = featureNamed(inputs, "cross_values");
    check(selfKeys != nullptr && crossValues != nullptr);

    if (selfKeys != nullptr && crossValues != nullptr)
    {
        check(selfKeys->shape == Vector<std::int64_t> {4, 448, 384});
        check(crossValues->shape == Vector<std::int64_t> {4, 1500, 384});
    }

    auto& outputs = specification.description.outputs;
    auto* logits = featureNamed(outputs, "logits");
    auto* newKeys = featureNamed(outputs, "new_keys");
    check(logits != nullptr && newKeys != nullptr);

    if (logits != nullptr && newKeys != nullptr)
    {
        check(logits->shape == Vector<std::int64_t> {1, 51864});
        check(newKeys->shape == Vector<std::int64_t> {4, 384});
    }

    check(countOf(specification, "gather") == 2);
    check(countOf(specification, "linear") == 4 * 8 + 1);
    check(countOf(specification, "matmul") == 8);
    check(countOf(specification, "softmax") == 4);
    check(countOf(specification, "scaled_dot_product_attention") == 4);
    check(countOf(specification, "layer_norm") == 13);
    check(countOf(specification, "gelu") == 4);

    auto text = graph.toText();
    check(text.find("decoder_3_self_attn_k_proj_weight") != std::string::npos);
    check(text.find("decoder_3_self_attn_k_proj_bias") == std::string::npos);
    check(text.find("embed_tokens") != std::string::npos);
    check(package.weights.size() > 2 * 20'000'000);
};
