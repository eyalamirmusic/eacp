#include "Common.h"

#include <eacp/GPU/Windows/D3D12PipelineCache.h>
#include <eacp/GPU/Windows/D3D12Types.h>

// D3D12's RenderPipelineCache: identical pipelines share one pipeline state,
// and a newer backend generation drops every state and never hands out one
// built under another. A rebuilt device is stood in for by a generation no
// device is at, which the next pipeline then replaces with the real one. Each
// case draws a red no other case in the process uses, so its states are its
// own. Self-skips without a device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace RenderPipelineCacheTests
{
struct CacheVertex
{
    float position[2];
};

constexpr CacheVertex cacheTriangle[] = {
    {{-1.f, -1.f}}, {{3.f, -1.f}}, {{-1.f, 3.f}}};

struct FlatShader final : ShaderProgram
{
    explicit FlatShader(float redToUse)
        : red(redToUse)
    {
        compile();
        setVertices(cacheTriangle);
    }

    void define() override
    {
        auto position = vertexInput(&CacheVertex::position);
        setPosition(float4(position, 0.f, 1.f));
        setFragment(float4(constant(red), 0.f, 0.f, 1.f));
    }

    float red;

    EACP_SHADER()
};

RenderPipelineDescriptor flatPipeline()
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.sampleCount = 1;
    return descriptor;
}

ID3D12PipelineState* stateOf(const ShaderProgram& program)
{
    return static_cast<D3D12Pipeline*>(program.pipeline().nativeState())
        ->state.get();
}

D3D12PipelineCache& sharedCache()
{
    return D3D12PipelineCache::of(Device::shared());
}

void followAnotherGeneration()
{
    RenderPipelineCache::follow(Device::shared());

    auto next = sharedCache().followed();
    next.generation += 1000;
    sharedCache().follow(next);
}

struct StatesSince
{
    int builds() const { return now().builds - before.builds; }
    int hits() const { return now().hits - before.hits; }

    static RenderPipelineCache::Stats now()
    {
        return RenderPipelineCache::stats(Device::shared());
    }

    RenderPipelineCache::Stats before = now();
};

auto tShares = test("RenderPipelineCache/identicalPipelinesShareOneState") = []
{
    if (!Device::shared().isValid())
        return;

    const auto states = StatesSince {};

    auto first = FlatShader {0.11f};
    first.prepare(flatPipeline());

    auto second = FlatShader {0.11f};
    second.prepare(flatPipeline());

    check(first.pipeline().isValid());
    check(second.pipeline().isValid());
    check(states.builds() == 1);
    check(states.hits() == 1);
    check(stateOf(first) == stateOf(second));

    auto blended = flatPipeline();
    blended.blendMode = BlendMode::AlphaBlend;

    auto third = FlatShader {0.11f};
    third.prepare(blended);

    check(third.pipeline().isValid());
    check(states.builds() == 2);
    check(stateOf(third) != stateOf(first));
};

auto tNewGeneration = test("RenderPipelineCache/aNewGenerationDropsEveryState") = []
{
    if (!Device::shared().isValid())
        return;

    auto first = FlatShader {0.21f};
    first.prepare(flatPipeline());

    check(first.pipeline().isValid());
    check(sharedCache().stats().entries > 0);

    followAnotherGeneration();

    check(sharedCache().stats().entries == 0);
    check(first.pipeline().isValid());

    const auto states = StatesSince {};
    auto second = FlatShader {0.21f};
    second.prepare(flatPipeline());

    check(second.pipeline().isValid());
    check(states.builds() == 1);
    check(states.hits() == 0);
};

auto tNeverHandedOut = test(
    "RenderPipelineCache/aStateBuiltUnderAnotherGenerationIsNeverHandedOut") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    followAnotherGeneration();

    auto warmed = FlatShader {0.31f};
    const auto library =
        ShaderLibraryCache::of(device).acquire(device, warmed.source());
    const auto descriptor = warmed.pipelineDescriptorFor(flatPipeline(), *library);

    check(descriptor.has_value());

    if (!descriptor.has_value())
        return;

    check(RenderPipelineCache::warm(device, *descriptor));

    const auto states = StatesSince {};
    auto prepared = FlatShader {0.31f};
    prepared.prepare(flatPipeline());

    check(prepared.pipeline().isValid());
    check(states.builds() == 1);
    check(states.hits() == 0);
};
} // namespace RenderPipelineCacheTests
