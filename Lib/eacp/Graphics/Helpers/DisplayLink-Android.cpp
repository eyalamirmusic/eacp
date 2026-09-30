#include "DisplayLink.h"

#include <eacp/Core/Threads/ThreadUtils.h>

#include <android/choreographer.h>

#include <memory>

namespace eacp::Threads
{
namespace
{
struct AndroidDisplayLinkTick
{
    explicit AndroidDisplayLinkTick(const Callback& cbToUse)
        : cb(cbToUse)
    {
    }

    Callback cb;
    bool alive = true;
};

using AndroidDisplayLinkTickPtr = std::shared_ptr<AndroidDisplayLinkTick>;

void androidPostDisplayLinkFrame(const AndroidDisplayLinkTickPtr& tick);

void androidDisplayLinkFrame(int64_t, void* data)
{
    auto tick = std::unique_ptr<AndroidDisplayLinkTickPtr>(
        static_cast<AndroidDisplayLinkTickPtr*>(data));

    if (!(*tick)->alive)
        return;

    (*tick)->cb();

    if ((*tick)->alive)
        androidPostDisplayLinkFrame(*tick);
}

void androidPostDisplayLinkFrame(const AndroidDisplayLinkTickPtr& tick)
{
    auto* choreographer = AChoreographer_getInstance();

    if (choreographer == nullptr)
        return;

    AChoreographer_postFrameCallback64(choreographer,
                                       androidDisplayLinkFrame,
                                       new AndroidDisplayLinkTickPtr {tick});
}
} // namespace

struct DisplayLink::Native
{
    explicit Native(const Callback& cb)
        : state(std::make_shared<AndroidDisplayLinkTick>(cb))
    {
        assertMainThread();
        androidPostDisplayLinkFrame(state);
    }

    ~Native()
    {
        assertMainThread();
        state->alive = false;
    }

    AndroidDisplayLinkTickPtr state;
};

DisplayLink::DisplayLink(const FrameCallback& cb)
    : rateLimit(std::make_shared<RateLimit>())
    , callback(rateLimited(rateLimit, timedTick(cb)))
    , impl(callback)
{
}

} // namespace eacp::Threads
