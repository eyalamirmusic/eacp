#pragma once

#include "../Utils/Common.h"

#include <exception>
#include <utility>

namespace eacp::Plugins
{
// The one object a plugin module hosts for its lifetime — the plugin-side
// mirror of Apps::run<T>. The host calls the module's exported "start" and the
// module builds its T; the host calls "stop" and the module destroys it:
//
//     EACP_PLUGIN_EXPORT int myPluginStart(const char* config)
//     {
//         return Plugins::start<Shell>(Options {config});
//     }
//
//     EACP_PLUGIN_EXPORT void myPluginStop()
//     {
//         Plugins::stop<Shell>();
//     }
//
// Both are noexcept, because both sit on an extern "C" boundary an exception
// must never cross, and because the host typically calls stop from its own
// teardown — often a destructor, where a throw would take the process down.
// So the two failure modes are reported the only ways they can be: a
// constructor that throws becomes a non-zero return, and a destructor that
// throws is logged and swallowed, there being nothing left to tell.
//
// Stop before the host unmaps the image (see Plugins::unload): the T owns the
// module's windows and threads, and they run the module's code. Storage is a
// function-local static, so a module that is unmapped without a stop still
// destroys its T during image teardown — a worse place to do it, but not a
// leak.
namespace Detail
{
template <typename T>
OwningPointer<T>& pluginObject()
{
    static auto object = OwningPointer<T> {};
    return object;
}
} // namespace Detail

// Constructs the module's T from `args`, replacing one already there. Returns
// 0 when it was constructed, non-zero when its constructor threw — the C
// convention the entry point returns to the host.
template <typename T, typename... Args>
int start(Args&&... args) noexcept
{
    try
    {
        Detail::pluginObject<T>().create(std::forward<Args>(args)...);
        return 0;
    }
    catch (const std::exception& e)
    {
        LOG("Plugin failed to start: ", e.what());
    }
    catch (...)
    {
        LOG("Plugin failed to start");
    }

    return 1;
}

// Destroys the module's T. Safe to call when it was never started, and safe to
// call twice.
template <typename T>
void stop() noexcept
{
    // Detached before it is destroyed, so a destructor that throws cannot
    // leave the module holding a pointer to a half-destroyed object — which
    // the next stop() would then delete a second time. The object is leaked
    // in that case: it failed to say what state it is in, so unwinding it
    // further is not something we can do safely.
    auto* object = Detail::pluginObject<T>().release();

    try
    {
        delete object;
    }
    catch (const std::exception& e)
    {
        LOG("Plugin failed to stop cleanly: ", e.what());
    }
    catch (...)
    {
        LOG("Plugin failed to stop cleanly");
    }
}

// The module's T, or nullptr when it is not running — for the entry points
// that have to reach it (a callback the host fires between start and stop).
template <typename T>
T* get() noexcept
{
    return Detail::pluginObject<T>().get();
}
} // namespace eacp::Plugins
