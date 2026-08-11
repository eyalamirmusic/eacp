#pragma once

#include "../Utils/Common.h"

#include <exception>
#include <utility>

namespace eacp::Plugins
{
// The one object a plugin module hosts for its lifetime — the plugin-side
// mirror of Apps::run<T>, driven by the module's exported start/stop. Both are
// noexcept: they sit on an extern "C" boundary. Stop before the host unmaps.
namespace Detail
{
template <typename T>
OwningPointer<T>& pluginObject()
{
    static auto object = OwningPointer<T> {};
    return object;
}
} // namespace Detail

// Replaces any T already there. Returns 0 on success, non-zero when the
// constructor threw — the C convention the entry point returns to the host.
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

// Safe to call when it was never started, and safe to call twice.
template <typename T>
void stop() noexcept
{
    // Detached first, so a throwing destructor leaks rather than leaving a
    // pointer to a half-destroyed object for the next stop() to delete again.
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

// nullptr when the module is not running.
template <typename T>
T* get() noexcept
{
    return Detail::pluginObject<T>().get();
}
} // namespace eacp::Plugins
