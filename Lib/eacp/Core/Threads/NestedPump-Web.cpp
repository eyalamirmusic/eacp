#include "EventLoop-Web.h"

#include <emscripten/em_js.h>

// clang-format off
EM_JS(void, eacpWebWakeFromScript, (), {
    var waiter = Module["eacpPumpWaiter"];
    if (waiter) {
        Module["eacpPumpWaiter"] = null;
        waiter();
    } else {
        Module["eacpPumpWakePending"] = true;
    }
});

EM_ASYNC_JS(void, eacpWebWaitInScript, (double timeoutMs), {
    if (Module["eacpPumpWakePending"]) {
        Module["eacpPumpWakePending"] = false;
        return;
    }
    await new Promise(function(resolve) {
        var timer = null;
        var finish = function() {
            Module["eacpPumpWaiter"] = null;
            if (timer !== null)
                clearTimeout(timer);
            resolve();
        };
        if (timeoutMs >= 0)
            timer = setTimeout(finish, timeoutMs);
        Module["eacpPumpWaiter"] = finish;
    });
});
// clang-format on

extern "C"
{
    bool eacpWebCanSuspend()
    {
        return true;
    }

    void eacpWebSuspendUntilWoken(double timeoutMs)
    {
        eacpWebWaitInScript(timeoutMs);
    }

    void eacpWebWakeSuspended()
    {
        eacpWebWakeFromScript();
    }
}
