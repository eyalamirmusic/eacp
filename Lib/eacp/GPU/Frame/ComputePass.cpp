#include "ComputePass.h"

namespace eacp::GPU
{
const ComputePass::Recorded& ComputePass::recorded() const
{
    return counts;
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
