#include "KernelCache.h"

#include "../Timing/CallCost.h"

#include <map>
#include <mutex>
#include <utility>

namespace eacp::GPU
{
namespace
{
struct KernelSlot
{
    std::once_flag built;
    std::unique_ptr<ComputeProgram> kernel;
};

struct KernelCacheStore
{
    std::mutex mutex;
    std::map<std::pair<std::type_index, std::vector<int>>,
             std::unique_ptr<KernelSlot>>
        slots;

    KernelSlot& slotFor(std::type_index type, std::vector<int> variant)
    {
        auto lock = std::scoped_lock {mutex};
        auto& slot = slots[{type, std::move(variant)}];

        if (slot == nullptr)
            slot = std::make_unique<KernelSlot>();

        return *slot;
    }
};

} // namespace

ComputeProgram& Detail::findOrBuildKernel(Device& device,
                                          std::type_index type,
                                          std::vector<int> variant,
                                          const KernelFactory& build)
{
    auto& slot =
        device.singleton<KernelCacheStore>().slotFor(type, std::move(variant));

    std::call_once(slot.built,
                   [&]
                   {
                       static auto builds = CallCostCounter {"kernel builds"};
                       auto cost = ScopedCallCost {builds};
                       slot.kernel = build();
                   });

    return *slot.kernel;
}

} // namespace eacp::GPU
