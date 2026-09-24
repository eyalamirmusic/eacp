#include "ComputePipelineCache.h"

#include "../Device/Device.h"

#include <map>
#include <mutex>
#include <string>

namespace eacp::GPU
{
namespace
{
struct CompiledComputeSlot
{
    std::once_flag built;
    std::shared_ptr<const CompiledCompute> compiled;
};

struct CompiledComputeStore
{
    std::mutex mutex;
    std::map<std::string, std::unique_ptr<CompiledComputeSlot>> slots;

    CompiledComputeSlot& slotFor(std::string key)
    {
        auto lock = std::scoped_lock {mutex};
        auto& slot = slots[std::move(key)];

        if (slot == nullptr)
            slot = std::make_unique<CompiledComputeSlot>();

        return *slot;
    }
};

std::string compiledComputeKey(const ShaderSource& source)
{
    auto key = std::to_string((int) source.backend) + '\n' + source.computeEntry
               + '\n' + std::to_string(source.threadGroup.x) + ','
               + std::to_string(source.threadGroup.y) + ','
               + std::to_string(source.threadGroup.z) + '\n';

    for (const auto& binding: source.bindings)
        key += std::to_string((int) binding.kind) + ':'
               + std::to_string((int) binding.stage) + ':'
               + std::to_string(binding.index) + ':' + binding.name + ';';

    key += '\n';
    key += source.source;

    return key;
}
} // namespace

CompiledCompute::CompiledCompute(Device& device, const ShaderSource& source)
    : library(device, source)
    , pipeline(device, library)
{
}

std::shared_ptr<const CompiledCompute>
    compileComputeCached(Device& device, const ShaderSource& source)
{
    auto& slot = device.singleton<CompiledComputeStore>().slotFor(
        compiledComputeKey(source));

    std::call_once(
        slot.built,
        [&] { slot.compiled = std::make_shared<CompiledCompute>(device, source); });

    return slot.compiled;
}
} // namespace eacp::GPU
