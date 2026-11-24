#include "D3D12PipelineCache.h"

#include "../Device/Device.h"
#include "D3D12Context.h"

#include <eacp/Core/Utils/Containers.h>
#include <eacp/Core/Utils/Sha256.h>

#include <condition_variable>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>

namespace eacp::GPU
{
namespace
{
using CacheKey = Sha256Digest;
using State = D3D12PipelineCache::State;

constexpr auto notYetFollowed = std::numeric_limits<std::uint64_t>::max();

struct CacheEntry
{
    State state;
    std::uint64_t generation = 0;
    bool ready = false;
};

using EntryPointer = std::shared_ptr<CacheEntry>;

void addBytes(Sha256& hash, const void* bytes, std::size_t size)
{
    hash.update(
        Span<const std::uint8_t>(static_cast<const std::uint8_t*>(bytes), size));
}

// One scalar at a time, never a struct whole, so no padding byte reaches the
// key.
template <typename T>
void addValue(Sha256& hash, T value)
{
    static_assert(std::is_scalar_v<T>);
    addBytes(hash, &value, sizeof(value));
}

void addBytecode(Sha256& hash, const D3D12_SHADER_BYTECODE& code)
{
    addValue(hash, code.BytecodeLength);

    if (code.pShaderBytecode != nullptr)
        addBytes(hash, code.pShaderBytecode, code.BytecodeLength);
}

void addBlend(Sha256& hash, const D3D12_BLEND_DESC& blend)
{
    addValue(hash, blend.AlphaToCoverageEnable);
    addValue(hash, blend.IndependentBlendEnable);

    for (const auto& target: blend.RenderTarget)
    {
        addValue(hash, target.BlendEnable);
        addValue(hash, target.LogicOpEnable);
        addValue(hash, target.SrcBlend);
        addValue(hash, target.DestBlend);
        addValue(hash, target.BlendOp);
        addValue(hash, target.SrcBlendAlpha);
        addValue(hash, target.DestBlendAlpha);
        addValue(hash, target.BlendOpAlpha);
        addValue(hash, target.LogicOp);
        addValue(hash, target.RenderTargetWriteMask);
    }
}

void addRasterizer(Sha256& hash, const D3D12_RASTERIZER_DESC& rasterizer)
{
    addValue(hash, rasterizer.FillMode);
    addValue(hash, rasterizer.CullMode);
    addValue(hash, rasterizer.FrontCounterClockwise);
    addValue(hash, rasterizer.DepthBias);
    addValue(hash, rasterizer.DepthBiasClamp);
    addValue(hash, rasterizer.SlopeScaledDepthBias);
    addValue(hash, rasterizer.DepthClipEnable);
    addValue(hash, rasterizer.MultisampleEnable);
    addValue(hash, rasterizer.AntialiasedLineEnable);
    addValue(hash, rasterizer.ForcedSampleCount);
    addValue(hash, rasterizer.ConservativeRaster);
}

void addStencilFace(Sha256& hash, const D3D12_DEPTH_STENCILOP_DESC& face)
{
    addValue(hash, face.StencilFailOp);
    addValue(hash, face.StencilDepthFailOp);
    addValue(hash, face.StencilPassOp);
    addValue(hash, face.StencilFunc);
}

void addDepthStencil(Sha256& hash, const D3D12_DEPTH_STENCIL_DESC& depth)
{
    addValue(hash, depth.DepthEnable);
    addValue(hash, depth.DepthWriteMask);
    addValue(hash, depth.DepthFunc);
    addValue(hash, depth.StencilEnable);
    addValue(hash, depth.StencilReadMask);
    addValue(hash, depth.StencilWriteMask);
    addStencilFace(hash, depth.FrontFace);
    addStencilFace(hash, depth.BackFace);
}

void addInputLayout(Sha256& hash, const D3D12_INPUT_LAYOUT_DESC& layout)
{
    addValue(hash, layout.NumElements);

    for (auto index = UINT {0}; index < layout.NumElements; ++index)
    {
        const auto& element = layout.pInputElementDescs[index];
        const auto semantic = std::string_view {
            element.SemanticName != nullptr ? element.SemanticName : ""};

        addValue(hash, semantic.size());
        addBytes(hash, semantic.data(), semantic.size());
        addValue(hash, element.SemanticIndex);
        addValue(hash, element.Format);
        addValue(hash, element.InputSlot);
        addValue(hash, element.AlignedByteOffset);
        addValue(hash, element.InputSlotClass);
        addValue(hash, element.InstanceDataStepRate);
    }
}

// Everything in the description but the root signature, which is the one the
// generation followed: a rebuilt device brings a new one, and a new generation
// with it.
CacheKey cacheKeyFor(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
    auto hash = Sha256 {};

    addBytecode(hash, desc.VS);
    addBytecode(hash, desc.PS);
    addBytecode(hash, desc.DS);
    addBytecode(hash, desc.HS);
    addBytecode(hash, desc.GS);
    addBlend(hash, desc.BlendState);
    addValue(hash, desc.SampleMask);
    addRasterizer(hash, desc.RasterizerState);
    addDepthStencil(hash, desc.DepthStencilState);
    addInputLayout(hash, desc.InputLayout);
    addValue(hash, desc.IBStripCutValue);
    addValue(hash, desc.PrimitiveTopologyType);
    addValue(hash, desc.NumRenderTargets);

    for (auto format: desc.RTVFormats)
        addValue(hash, format);

    addValue(hash, desc.DSVFormat);
    addValue(hash, desc.SampleDesc.Count);
    addValue(hash, desc.SampleDesc.Quality);
    addValue(hash, desc.NodeMask);
    addValue(hash, desc.Flags);

    return hash.finish();
}

State buildState(ID3D12Device* device,
                 const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
    auto state = State {};

    if (device != nullptr)
        device->CreateGraphicsPipelineState(
            &desc, __uuidof(ID3D12PipelineState), state.put_void());

    return state;
}
} // namespace

struct D3D12PipelineCache::Impl
{
    bool isUsable(const CacheEntry& entry) const
    {
        return entry.ready && entry.state != nullptr
               && entry.generation == current.generation;
    }

    // The state leaves the entry under the lock and is released by the
    // caller once it is unlocked, on the owning thread.
    static void evict(CacheEntry& entry, Vector<State>& evicted)
    {
        auto taken = State {};
        std::swap(taken, entry.state);
        evicted.add(taken);
    }

    void forgetOlderGenerations(std::uint64_t generation, Vector<State>& evicted)
    {
        if (generation == current.generation)
            return;

        for (auto it = entries.begin(); it != entries.end();)
        {
            if (it->second->ready)
            {
                evict(*it->second, evicted);
                it = entries.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    EntryPointer claim(const CacheKey& key, std::uint64_t generation)
    {
        auto entry = std::make_shared<CacheEntry>();
        entry->generation = generation;
        entries[key] = entry;
        ++counts.builds;
        return entry;
    }

    void waitUntilReady(std::unique_lock<std::mutex>& lock,
                        const EntryPointer& entry)
    {
        auto isReady = [&entry] { return entry->ready; };
        finished.wait(lock, isReady);
    }

    // A state that did not build leaves no entry behind, so the next request
    // builds it again.
    void publish(const CacheKey& key, const EntryPointer& entry, State& built)
    {
        std::swap(entry->state, built);
        entry->ready = true;

        if (entry->state == nullptr)
            if (auto found = entries.find(key);
                found != entries.end() && found->second == entry)
                entries.erase(found);

        finished.notify_all();
    }

    EntryPointer find(const CacheKey& key) const
    {
        auto found = entries.find(key);
        return found != entries.end() ? found->second : nullptr;
    }

    mutable std::mutex mutex;
    std::condition_variable finished;
    std::map<CacheKey, EntryPointer> entries;
    Followed current {nullptr, nullptr, notYetFollowed};
    RenderPipelineCache::Stats counts;
};

D3D12PipelineCache::D3D12PipelineCache() = default;

D3D12PipelineCache::~D3D12PipelineCache() = default;

D3D12PipelineCache& D3D12PipelineCache::of(Device& device)
{
    return device.perDevice<D3D12PipelineCache>();
}

void D3D12PipelineCache::follow(Device& device)
{
    const auto& shared = getD3D12Shared();

    follow(Followed {shared.deviceReference(),
                     shared.renderRootSignatureReference(),
                     device.backendGeneration()});
}

void D3D12PipelineCache::follow(const Followed& next)
{
    auto evicted = Vector<State> {};
    auto previous = next;
    auto lock = std::lock_guard {impl->mutex};

    impl->forgetOlderGenerations(next.generation, evicted);
    std::swap(impl->current, previous);
}

D3D12PipelineCache::Followed D3D12PipelineCache::followed() const
{
    auto lock = std::lock_guard {impl->mutex};
    return impl->current;
}

D3D12PipelineCache::State
    D3D12PipelineCache::acquire(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
    const auto key = cacheKeyFor(desc);
    auto evicted = Vector<State> {};
    auto lock = std::unique_lock {impl->mutex};

    if (auto existing = impl->find(key))
    {
        if (!existing->ready)
        {
            ++impl->counts.waits;
            impl->waitUntilReady(lock, existing);
        }

        if (impl->isUsable(*existing))
        {
            ++impl->counts.hits;
            return existing->state;
        }

        Impl::evict(*existing, evicted);
    }

    const auto against = impl->current;
    auto entry = impl->claim(key, against.generation);
    lock.unlock();

    auto built = buildState(against.device.get(), desc);
    auto result = built;

    lock.lock();
    impl->publish(key, entry, built);
    return result;
}

bool D3D12PipelineCache::warm(const Followed& against,
                              const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
    const auto key = cacheKeyFor(desc);
    auto lock = std::unique_lock {impl->mutex};

    if (against.generation != impl->current.generation)
        return false;

    if (auto existing = impl->find(key))
    {
        if (!existing->ready)
        {
            ++impl->counts.waits;
            impl->waitUntilReady(lock, existing);
        }

        if (!impl->isUsable(*existing))
            return false;

        ++impl->counts.hits;
        return true;
    }

    auto entry = impl->claim(key, against.generation);
    lock.unlock();

    auto built = buildState(against.device.get(), desc);

    lock.lock();
    impl->publish(key, entry, built);
    return entry->state != nullptr;
}

RenderPipelineCache::Stats D3D12PipelineCache::stats() const
{
    auto lock = std::lock_guard {impl->mutex};
    auto result = impl->counts;
    result.entries = static_cast<int>(impl->entries.size());
    return result;
}
} // namespace eacp::GPU
