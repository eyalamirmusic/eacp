#include "OcclusionQueries.h"

#include "../Device/Device.h"

// Device::supportsOcclusionQueries is false on the web for now: a set is never
// valid, and a pass naming one counts nothing (WebGPU's occlusion query sets
// are not wired up yet).

namespace eacp::GPU
{
struct OcclusionQueries::Native
{
};

OcclusionQueries::OcclusionQueries(Device&, int) {}

OcclusionQueries::~OcclusionQueries() = default;

bool OcclusionQueries::isValid() const
{
    return false;
}

int OcclusionQueries::size() const
{
    return 0;
}

bool OcclusionQueries::isComplete() const
{
    return true;
}

void OcclusionQueries::read(std::span<std::uint64_t>, int) const {}

void* OcclusionQueries::nativeResults() const
{
    return nullptr;
}

void* OcclusionQueries::nativeQueries() const
{
    return nullptr;
}

void OcclusionQueries::notePass(void*) {}
} // namespace eacp::GPU
