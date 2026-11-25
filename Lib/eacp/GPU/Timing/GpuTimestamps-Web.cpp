#include "GpuTimestamps.h"

// No per-pass GPU timing on the web yet: WebGPU's timestamp queries are not
// wired up, so isSupported is false and the per-pass breakdown stays empty.

namespace eacp::GPU
{
struct GpuTimestamps::Native
{
};

GpuTimestamps::GpuTimestamps() = default;
GpuTimestamps::~GpuTimestamps() = default;

bool GpuTimestamps::isSupported() const
{
    return false;
}

void GpuTimestamps::beginSlot(int, Device&) {}

void GpuTimestamps::beginRecording(int, void*) {}

void* GpuTimestamps::nativeSamples(int) const
{
    return nullptr;
}

bool GpuTimestamps::endSlot(int, int, void*)
{
    return false;
}

void GpuTimestamps::noteSubmitted(int, std::uint64_t) {}

bool GpuTimestamps::isSlotComplete(int, const Device&) const
{
    return true;
}

double GpuTimestamps::resolveSlot(int, int, PassActivity*)
{
    return 0.0;
}
} // namespace eacp::GPU
