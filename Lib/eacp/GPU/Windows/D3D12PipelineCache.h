#pragma once

#include "../Pipeline/RenderPipelineCache.h"

#include <eacp/Core/Utils/Pimpl.h>
#include <eacp/Core/Utils/WinInclude.h>

#include <d3d12.h>

#include <winrt/base.h>

#include <cstdint>

// The D3D12 half of RenderPipelineCache: one per GPU::Device
// (Device::perDevice), destroyed with it. Not part of GPU.h.
//
// A state leaves the cache only on the Device's owning thread - when follow()
// meets a newer backend generation - or with the cache itself. Releasing the
// cache's reference is safe whenever that happens: every RenderPipeline holds
// a reference of its own and defers its release through D3D12Context, so no
// recorded command list is ever left holding the last one.

namespace eacp::GPU
{
class Device;

class D3D12PipelineCache
{
public:
    using State = winrt::com_ptr<ID3D12PipelineState>;

    // What states are built against, and the generation they are stamped
    // with: references of their own, so a worker building with them is not
    // racing the recovery that replaces the shared device.
    struct Followed
    {
        winrt::com_ptr<ID3D12Device> device;
        winrt::com_ptr<ID3D12RootSignature> renderRootSignature;
        std::uint64_t generation = 0;
    };

    D3D12PipelineCache();
    ~D3D12PipelineCache();

    D3D12PipelineCache(const D3D12PipelineCache&) = delete;
    D3D12PipelineCache& operator=(const D3D12PipelineCache&) = delete;

    static D3D12PipelineCache& of(Device& device);

    // On the owning thread: follows the shared device, its render root
    // signature and Device::backendGeneration.
    void follow(Device& device);

    // The same with what to follow given, on the owning thread. A generation
    // no device is at stands in for a rebuilt device, which is how the tests
    // drive it.
    void follow(const Followed& next);

    // From any thread: what was followed last.
    Followed followed() const;

    // On the owning thread, after follow(): the state for `desc`, which names
    // the root signature followed, taken from the cache, waited for, or built
    // now and kept. Null when it does not build.
    State acquire(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);

    // From any thread: builds the state for `desc` against `against` - a
    // followed() snapshot, whose root signature `desc` names - unless it is
    // there or in flight. False when it does not build, or when `against` is
    // no longer the generation followed.
    bool warm(const Followed& against,
              const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);

    RenderPipelineCache::Stats stats() const;

private:
    struct Impl;
    Pimpl<Impl> impl;
};
} // namespace eacp::GPU
