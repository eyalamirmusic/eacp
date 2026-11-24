#include "RenderPipelineCache.h"

namespace eacp::GPU
{
bool RenderPipelineCache::isSupported()
{
    return false;
}

void RenderPipelineCache::follow(Device&) {}

bool RenderPipelineCache::warm(Device&, const RenderPipelineDescriptor&)
{
    return false;
}

RenderPipelineCache::Stats RenderPipelineCache::stats(Device&)
{
    return {};
}
} // namespace eacp::GPU
