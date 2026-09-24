#include "BufferPool.h"

#include "../Device/Device.h"

#include <algorithm>

namespace eacp::GPU
{
BufferPool& BufferPool::of(Device& device)
{
    auto& pool = device.perDevice<BufferPool>();
    pool.device = &device;

    return pool;
}

Buffer BufferPool::take(std::int64_t bytes, BufferUsage usage)
{
    promoteFinished();
    freeUnused();

    auto found = available.find(Key {bytes, usage});
    auto reused = found != available.end();

    auto buffer = reused ? std::move(found->second.buffer)
                         : Buffer {*device, nullptr, bytes, usage};

    if (reused)
    {
        availableBytes -= found->first.first;
        available.erase(found);
    }

    if (buffer.isValid())
    {
        buffer.pool = this;
        buffer.pooledUsage = usage;
    }

    return buffer;
}

void BufferPool::give(Buffer storage, Key key)
{
    if (device == nullptr)
        return;

    waiting.push_back(Waiting {.freeAfter = device->lastSubmission() + 1,
                               .key = key,
                               .buffer = std::move(storage)});
}

void BufferPool::promoteFinished()
{
    while (!waiting.empty() && device->hasFinished(waiting.front().freeAfter))
    {
        auto& front = waiting.front();

        availableBytes += front.key.first;
        available.emplace(front.key,
                          Available {.since = device->lastSubmission(),
                                     .buffer = std::move(front.buffer)});
        waiting.pop_front();
    }

    freeOldestBeyondBudget();
}

// The bound the submission rule cannot give: least recently returned first,
// until what is held fits. Only storage nothing is waiting on is dropped, so
// this never takes a buffer the GPU could still be reading.
// A quarter of what the device recommends keeping resident, capped at what the
// work reuses. The cap is what usually applies; the quarter is what stops a
// small card being asked to hold a share of itself it has not got. A backend
// that will not say answers zero, and then the cap is the whole rule.
std::int64_t BufferPool::bytesKeptUnused() const
{
    // Asked once. Every take() checks the bound, and on D3D12 the answer costs
    // a DXGI factory and an adapter enumeration - which, asked per allocation,
    // is far more than the allocation it is there to save.
    if (bound == 0)
    {
        auto recommended = device != nullptr ? device->memoryBudget() : 0;

        bound = recommended > 0
                  ? std::min(bytesKeptUnusedCeiling, recommended / 4)
                  : bytesKeptUnusedCeiling;
    }

    return bound;
}

void BufferPool::freeOldestBeyondBudget()
{
    auto budget = bytesKeptUnused();

    while (availableBytes > budget && !available.empty())
    {
        auto oldest = available.begin();

        for (auto it = available.begin(); it != available.end(); ++it)
            if (it->second.since < oldest->second.since)
                oldest = it;

        availableBytes -= oldest->first.first;
        available.erase(oldest);
    }
}

void BufferPool::freeUnused()
{
    auto now = device->lastSubmission();

    if (now == lastTrimmed)
        return;

    lastTrimmed = now;

    std::erase_if(available,
                  [this, now](const auto& entry)
                  {
                      auto stale = entry.second.since + submissionsKeptUnused < now;

                      if (stale)
                          availableBytes -= entry.first.first;

                      return stale;
                  });
}
} // namespace eacp::GPU
