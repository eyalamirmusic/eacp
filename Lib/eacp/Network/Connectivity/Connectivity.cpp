#include "ConnectivityInternal.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Threads/ThreadUtils.h>
#include <eacp/Core/Utils/Singleton.h>
#include <eacp/Network/HTTP/Http.h>

#include <mutex>
#include <thread>

namespace eacp::Network::Connectivity
{
namespace
{
// Not Monitor::get(): that one starts the monitor, and the start reports a
// seed state through here before it returns.
Monitor& instance()
{
    return Singleton::get<Monitor>();
}

void ensureMonitoring()
{
    static auto started = std::once_flag {};

    std::call_once(started,
                   []
                   {
                       instance();
                       detail::startPlatformMonitoring();
                   });
}

bool answeredAsExpected(const HTTP::Response& response,
                        const std::string& expectedContent)
{
    if (!response.error.empty())
        return false;

    if (response.statusCode < 200 || response.statusCode >= 300)
        return false;

    return expectedContent.empty()
           || response.content.find(expectedContent) != std::string::npos;
}
} // namespace

namespace detail
{
void reportState(const State& state)
{
    if (Threads::isMainThread())
    {
        instance().setState(state);
        return;
    }

    Threads::callAsync([state] { instance().setState(state); });
}
} // namespace detail

// Apple's over https: App Transport Security refuses plain http from
// NSURLSession, and no eacp bundle carries an exemption. A portal in the way
// then fails the TLS handshake instead of answering with its login page,
// which comes out the same.
std::string ProbeOptions::defaultUrl()
{
#if defined(_WIN32)
    return "http://www.msftconnecttest.com/connecttest.txt";
#elif defined(__APPLE__)
    return "https://captive.apple.com/hotspot-detect.html";
#else
    return "http://detectportal.firefox.com/success.txt";
#endif
}

std::string ProbeOptions::defaultExpectedContent()
{
#if defined(_WIN32)
    return "Microsoft Connect Test";
#elif defined(__APPLE__)
    return "Success";
#else
    return "success";
#endif
}

Monitor& Monitor::get()
{
    ensureMonitoring();
    return instance();
}

void Monitor::setState(const State& newState)
{
    auto wasOnline = reported.online;
    reported = newState;
    publish();

    if (probing && reported.online && !wasOnline)
        scheduleProbe(Time::MS {0});
}

void Monitor::publish()
{
    auto newState = reported;
    newState.reachable =
        probing ? reported.online && lastProbeSucceeded : reported.online;

    if (state == newState)
        return;

    state = newState;
    trigger();
}

void Monitor::startProbe(const ProbeOptions& options)
{
    probeOptions = options;
    probing = true;
    lastProbeSucceeded = true;
    publish();
    scheduleProbe(Time::MS {0});
}

void Monitor::stopProbe()
{
    probing = false;
    ++probeGeneration;
    publish();
}

void Monitor::probeNow()
{
    if (probing)
        scheduleProbe(Time::MS {0});
}

// callAfter cannot be cancelled, so every schedule and every stop bumps the
// generation and a callback from before the bump does nothing. The monitor
// is a process singleton, so the pointer the callbacks reach is always live.
void Monitor::scheduleProbe(Time::MS delay)
{
    auto generation = ++probeGeneration;

    auto fire = [this, generation]
    {
        if (generation == probeGeneration && probing)
            runProbe();
    };

    if (delay.count <= 0)
        fire();
    else
        Threads::callAfter(delay, fire);
}

// Its own thread rather than HTTP::asyncRequest: cancelAllAsyncRequests
// would silently end the chain, and the generation already guards a stale
// reply.
void Monitor::runProbe()
{
    if (!reported.online)
        return;

    auto request = HTTP::Request(probeOptions.url);
    request.timeout = probeOptions.timeout;

    auto generation = probeGeneration;
    auto expected = probeOptions.expectedContent;

    auto fetch = [request, expected, generation]
    {
        auto succeeded = answeredAsExpected(request.perform(), expected);

        Threads::callAsync([generation, succeeded]
                           { instance().onProbeResult(generation, succeeded); });
    };

    auto worker = std::thread(fetch);
    worker.detach();
}

void Monitor::onProbeResult(int generation, bool succeeded)
{
    if (generation != probeGeneration || !probing)
        return;

    lastProbeSucceeded = succeeded;
    publish();

    scheduleProbe(succeeded ? probeOptions.interval : probeOptions.retryInterval);
}

std::string toString(Interface interfaceKind)
{
    switch (interfaceKind)
    {
        case Interface::Wifi:
            return "Wi-Fi";
        case Interface::Cellular:
            return "Cellular";
        case Interface::Wired:
            return "Wired";
        case Interface::Other:
            return "Other";
        case Interface::None:
            break;
    }

    return "None";
}

const State& getState()
{
    return Monitor::get().getState();
}

bool isThisMachineOnline()
{
    return getState().online;
}

bool hasInternet()
{
    return getState().hasInternet();
}
} // namespace eacp::Network::Connectivity
