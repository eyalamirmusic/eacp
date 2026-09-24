#include "KernelCache.h"

#include <algorithm>
#include <atomic>
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

int warmupThreadCount(std::size_t taskCount)
{
    auto cores = (int) std::thread::hardware_concurrency();
    auto wanted = std::clamp(cores / 2, 1, 8);

    return std::min(wanted, (int) taskCount);
}
} // namespace

ComputeProgram& Detail::findOrBuildKernel(Device& device,
                                          std::type_index type,
                                          std::vector<int> variant,
                                          const KernelFactory& build)
{
    auto& slot =
        device.singleton<KernelCacheStore>().slotFor(type, std::move(variant));

    std::call_once(slot.built, [&] { slot.kernel = build(); });

    return *slot.kernel;
}

KernelWarmup::~KernelWarmup()
{
    wait();
}

void KernelWarmup::start(Device& device)
{
    auto next = std::make_shared<std::atomic<std::size_t>>(0);
    auto count = warmupThreadCount(tasks.size());

    for (auto i = 0; i < count; ++i)
        workers.emplace_back(
            [this, next, &device]
            {
                for (auto task = next->fetch_add(1); task < tasks.size();
                     task = next->fetch_add(1))
                    tasks[task](device);
            });
}

void KernelWarmup::wait()
{
    for (auto& worker: workers)
        worker.join();

    workers.clear();
}
} // namespace eacp::GPU
