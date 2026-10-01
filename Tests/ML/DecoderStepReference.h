#pragma once

#include "WhisperDecoderStep.h"

namespace WhisperDecoderStep
{
struct StepOutputs
{
    Vector<float> logits;
    Vector<float> newKeys;
    Vector<float> newValues;
};

// The step WhisperDecoderStep::Builder records, in fp32 on the CPU: the self
// attention reads the first inputs.prefix cache rows and this token's own.
StepOutputs referenceStep(const Weights& weights, const StepInputs& inputs);
} // namespace WhisperDecoderStep
