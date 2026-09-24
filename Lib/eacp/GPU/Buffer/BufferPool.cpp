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
        available.erase(found);

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

        available.emplace(front.key,
                          Available {.since = device->lastSubmission(),
                                     .buffer = std::move(front.buffer)});
        waiting.pop_front();
    }
}

void BufferPool::freeUnused()
{
    auto now = device->lastSubmission();

    if (now == lastTrimmed)
        return;

    lastTrimmed = now;

    std::erase_if(available,
                  [now](const auto& entry)
                  { return entry.second.since + submissionsKeptUnused < now; });
}
} // namespace eacp::GPU
