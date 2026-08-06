#pragma once

#include <Miro/Bridge.h>
#include <Miro/Reflect.h>

#include <functional>

// PanelState stays at file scope so its qualifiedName matches the
// generated TS exactly; the defaults double as the hooks' initial value.
struct PanelState
{
    long long ticks = 0;
    long long pageClicks = 0;
    long long heartbeats = 0;

    MIRO_REFLECT(ticks, pageClicks, heartbeats)
};

namespace Api
{

// The bridged surface of one open/close cycle. A fresh instance is
// created with every panel, so the page always starts from zeroed
// state after a reopen — which is what makes reopening observably a
// new WebView rather than a rehidden one.
//
// All method bodies are inline because the codegen executable ODR-uses
// the pmfs through the makePmfHandler lambda chain — and the codegen
// exe doesn't compile any .cpp in this app.
class PanelApi
{
public:
    void reflect(Miro::ApiReflector& r)
    {
        using T = PanelApi;

        r.commands<&T::getState, &T::pingFromPage, &T::heartbeat>();
        r.events<&T::state>();
    }

    PanelState getState() const { return state.snapshot(); }

    void pingFromPage()
    {
        auto next = state.snapshot();
        next.pageClicks++;
        state.publish(next);
        onPageClick(next.pageClicks);
    }

    // Called by the page every 50ms while it is alive, so destroying the
    // WebView always happens with page->native commands in flight — the
    // exact traffic the bridge's teardown guards have to drop safely.
    void heartbeat()
    {
        auto next = state.snapshot();
        next.heartbeats++;
        state.publish(next);
    }

    // Called from the panel's native timer — not exposed as a command.
    void advanceTick()
    {
        auto next = state.snapshot();
        next.ticks++;
        state.publish(next);
    }

    std::function<void(long long)> onPageClick = [](long long) {};

    Miro::Event<PanelState> state;
};

} // namespace Api
