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

template <typename Fn>
auto whenAlive(const std::weak_ptr<Monitor*>& token, Fn fn)
{
    return [token, fn]
    {
        if (auto live = token.lock())
            fn(**live);
    };
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

// The optimism is for a first start only: a restart with new options keeps
// the verdict the last fetch reached rather than flashing green until the
// next one.
void Monitor::startProbe(const ProbeOptions& options)
{
    probeOptions = options;

    if (!probing)
        lastProbeSucceeded = true;

    probing = true;
    probeInFlight = false;
    publish();
    scheduleProbe(Time::MS {0});
}

void Monitor::stopProbe()
{
    probing = false;
    probeInFlight = false;
    ++probeGeneration;
    publish();
}

// A fetch already under way is the answer being asked for: it lands within
// the timeout and restarts the schedule itself, so it is not doubled up.
void Monitor::probeNow()
{
    if (probing && !probeInFlight)
        scheduleProbe(Time::MS {0});
}

// callAfter cannot be cancelled, so every schedule and every stop bumps the
// generation and a callback from before the bump does nothing.
void Monitor::scheduleProbe(Time::MS delay)
{
    auto generation = ++probeGeneration;

    if (delay.count <= 0)
    {
        onProbeDue(generation);
        return;
    }

    auto token = std::weak_ptr(alive);

    Threads::callAfter(delay,
                       whenAlive(token,
                                 [generation](Monitor& monitor)
                                 { monitor.onProbeDue(generation); }));
}

void Monitor::onProbeDue(int generation)
{
    if (generation == probeGeneration && probing)
        runProbe();
}

// Its own thread rather than HTTP::asyncRequest: cancelAllAsyncRequests
// would silently end the chain, and the generation already guards a stale
// reply. no-cache because Apple's endpoint answers with a year's max-age: a
// client cache that honoured it would keep saying reachable with the uplink
// gone.
void Monitor::runProbe()
{
    if (!reported.online)
        return;

    auto request = HTTP::Request(probeOptions.url);
    request.timeout = probeOptions.timeout;
    request.headers["Cache-Control"] = "no-cache";

    auto generation = probeGeneration;
    auto expected = probeOptions.expectedContent;
    auto token = std::weak_ptr(alive);
    probeInFlight = true;

    auto fetch = [request, expected, generation, token]
    {
        auto succeeded = answeredAsExpected(request.perform(), expected);

        Threads::callAsync(
            whenAlive(token,
                      [generation, succeeded](Monitor& monitor)
                      { monitor.onProbeResult(generation, succeeded); }));
    };

    auto worker = std::thread(fetch);
    worker.detach();
}

void Monitor::onProbeResult(int generation, bool succeeded)
{
    if (generation != probeGeneration || !probing)
        return;

    probeInFlight = false;
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
