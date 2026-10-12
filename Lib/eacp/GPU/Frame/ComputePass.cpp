#include "ComputePass.h"

namespace eacp::GPU
{
const ComputePass::Recorded& ComputePass::recorded() const
{
    return counts;
}

ThreadGroupShape ComputePass::groupFor1D() const
{
    return boundGroup.isSet() ? boundGroup
                              : ThreadGroupShape {threadGroupWidth, 1, 1};
}

ThreadGroupShape ComputePass::groupFor2D() const
{
    return boundGroup.isSet()
               ? boundGroup
               : ThreadGroupShape {threadGroupSize2D, threadGroupSize2D, 1};
}

ThreadGroupShape ComputePass::groupFor3D() const
{
    return boundGroup.isSet() ? boundGroup
                              : ThreadGroupShape {threadGroupSize3D,
                                                  threadGroupSize3D,
                                                  threadGroupSize3D};
}

void ComputePass::setPipeline(const ComputePipeline& pipeline)
{
    ++counts.pipelineSets;
    encodePipeline(pipeline);
}

void ComputePass::dispatch(int count)
{
    ++counts.dispatches;
    encodeDispatch(count);
}

void ComputePass::dispatch(int width, int height)
{
    ++counts.dispatches;
    encodeDispatch(width, height);
}

void ComputePass::dispatch(int width, int height, int depth)
{
    ++counts.dispatches;
    encodeDispatch(width, height, depth);
}

void ComputePass::dispatchIndirect(const Buffer& arguments,
                                   std::int64_t offsetInBytes)
{
    ++counts.indirectDispatches;
    encodeDispatchIndirect(arguments, offsetInBytes);
}

void ComputePass::barrier()
{
    ++counts.barriers;
    encodeBarrier();
}
} // namespace eacp::GPU
