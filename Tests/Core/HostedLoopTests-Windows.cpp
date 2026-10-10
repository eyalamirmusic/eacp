// A hosted eacp copy under a foreign host's pump: attachCurrentThreadAsMain
// brings the message-only window up, the host's own PeekMessage loop drives it,
// and detachCurrentThreadAsMain takes everything down again before the host
// unloads the image. The pump here is plain Win32 on purpose: runEventLoopFor
// would bring eacp's own loop services back up and hide what a DAW sees.

#include "Common.h"

#include <eacp/Core/Utils/WinInclude.h>

using namespace nano;
using eacp::Threads::attachCurrentThreadAsMain;
using eacp::Threads::callAfter;
using eacp::Threads::callAsync;
using eacp::Threads::detachCurrentThreadAsMain;

namespace
{
void pumpAsAHostWould(eacp::Time::MS duration)
{
    auto deadline = eacp::Time::Deadline {duration};

    while (!deadline.expired())
    {
        auto msg = MSG {};

        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        Sleep(5);
    }
}
} // namespace

auto tHostPumpRunsAttachedCallbacks =
    test("HostedLoop/aHostsOwnPumpRunsCallbacksWhileAttached") = []
{
    attachCurrentThreadAsMain();

    auto ran = false;
    auto delayed = false;
    callAsync([&] { ran = true; });
    callAfter(eacp::Time::MS {20}, [&] { delayed = true; });

    pumpAsAHostWould(eacp::Time::MS {300});
    check(ran);
    check(delayed);

    detachCurrentThreadAsMain();
};

// What the host's pump dispatched after an unload would have landed in freed
// code: the window, the scheduler and the queue must all be gone.
auto tDetachLeavesNothing = test("HostedLoop/detachLeavesNothingForTheHostsPump") = []
{
    attachCurrentThreadAsMain();

    auto ran = false;
    callAsync([&] { ran = true; });
    callAfter(eacp::Time::MS {20}, [&] { ran = true; });

    detachCurrentThreadAsMain();
    pumpAsAHostWould(eacp::Time::MS {200});
    check(!ran);
};

// A host that scans, unloads and loads again attaches twice in one process.
auto tReattachStartsOver = test("HostedLoop/aLaterAttachWorksAgain") = []
{
    attachCurrentThreadAsMain();
    detachCurrentThreadAsMain();
    attachCurrentThreadAsMain();

    auto ran = false;
    auto delayed = false;
    callAsync([&] { ran = true; });
    callAfter(eacp::Time::MS {20}, [&] { delayed = true; });

    pumpAsAHostWould(eacp::Time::MS {300});
    check(ran);
    check(delayed);

    detachCurrentThreadAsMain();
};
