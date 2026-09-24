#include "BufferPool.h"

#include "../Device/Device.h"

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
void BufferPool::freeOldestBeyondBudget()
{
    while (availableBytes > bytesKeptUnused && !available.empty())
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
