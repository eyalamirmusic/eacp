#pragma once

namespace eacp::GPU
{
class Device;
struct RenderPipelineDescriptor;

// The pipeline states one Device built, kept for an identical request to take
// rather than build again. Only D3D12 keeps one: Metal's driver already serves
// an identical request from a cache of its own, and D3D12's does not, so there
// every RenderPipeline takes its state from here and a ShaderWarmUp's worker
// can build one ahead of the main thread. A state is keyed by the whole
// description it was built from and the bytes of each shader stage, and is
// stamped with the backend generation it was built under
// (Device::backendGeneration), so one from before the device was rebuilt is
// never handed out. Elsewhere every call does nothing.
namespace RenderPipelineCache
{
struct Stats
{
    // Pipeline states built through the cache. One that fails to build is not
    // kept, and is built again when it is asked for.
    int builds = 0;

    // Requests answered by a state already there, warm() included.
    int hits = 0;

    // Requests that found the state in flight on another thread and waited
    // for it rather than building it twice.
    int waits = 0;

    // States held now.
    int entries = 0;
};

// Whether this backend keeps one.
bool isSupported();

// On the Device's owning thread: takes up the backend generation and the
// native device it is at now, dropping states from an older generation.
// warm() builds against what was followed last, so call this before warming
// from another thread; ShaderWarmUp::start() does, and every RenderPipeline
// does first.
void follow(Device& device);

// From any thread: builds the state `descriptor` describes into the cache
// unless it is there already or in flight elsewhere, which it waits for. True
// when a usable state is there afterwards. Whatever it builds stays in the
// cache, so it never releases a state; false where the backend keeps no cache.
bool warm(Device& device, const RenderPipelineDescriptor& descriptor);

Stats stats(Device& device);
} // namespace RenderPipelineCache
} // namespace eacp::GPU
