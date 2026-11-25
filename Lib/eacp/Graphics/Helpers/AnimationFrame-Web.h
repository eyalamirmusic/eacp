#pragma once

#include "../Common.h"

namespace eacp::Graphics
{
// One requestAnimationFrame at a time, or a 60 Hz timeout under node, which
// has none. Holds no runtime keepalive, as the loop's own timers do not, and
// is cancelled when the runtime exits. The callback runs inside the browser's
// frame callback, not on a later turn.
class AnimationFrame
{
public:
    AnimationFrame();
    ~AnimationFrame();

    AnimationFrame(const AnimationFrame&) = delete;
    AnimationFrame& operator=(const AnimationFrame&) = delete;

    // Replaces the callback of a request still pending.
    void request(Callback onFrame);
    void cancel();

    constexpr bool isPending() const { return pending; }

    void fire();

private:
    int id;
    bool pending = false;
    Callback callback = [] {};
};
} // namespace eacp::Graphics
