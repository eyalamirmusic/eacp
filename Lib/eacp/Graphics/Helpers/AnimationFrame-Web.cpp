#include "AnimationFrame-Web.h"

#include "../Window/WebWindowSystem-Web.h"

#include <emscripten/emscripten.h>

#include <unordered_map>
#include <utility>

namespace eacp::Graphics
{
namespace
{
std::unordered_map<int, AnimationFrame*>& webAnimationFrames()
{
    static auto frames = std::unordered_map<int, AnimationFrame*> {};
    return frames;
}

int nextWebAnimationFrameId()
{
    static auto next = 0;
    return ++next;
}
} // namespace

AnimationFrame::AnimationFrame()
    : id(nextWebAnimationFrameId())
{
    webAnimationFrames()[id] = this;
}

AnimationFrame::~AnimationFrame()
{
    cancel();
    webAnimationFrames().erase(id);
}

void AnimationFrame::request(Callback onFrame)
{
    callback = std::move(onFrame);

    if (std::exchange(pending, true))
        return;

    eacp_web_frame_request(id);
}

void AnimationFrame::cancel()
{
    if (!std::exchange(pending, false))
        return;

    callback = [] {};
    eacp_web_frame_cancel(id);
}

// Moved out first: the callback may request the next frame, or destroy this.
void AnimationFrame::fire()
{
    if (!std::exchange(pending, false))
        return;

    auto onFrame = std::exchange(callback, [] {});
    onFrame();
}
} // namespace eacp::Graphics

extern "C"
{
    EMSCRIPTEN_KEEPALIVE void eacp_web_frame(int id)
    {
        auto& frames = eacp::Graphics::webAnimationFrames();
        auto found = frames.find(id);

        if (found != frames.end())
            found->second->fire();
    }
}
