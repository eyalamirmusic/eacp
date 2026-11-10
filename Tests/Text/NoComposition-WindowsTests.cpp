#include "Common.h"

#include <eacp/Core/Threads/CallAfter.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Logging.h>
#include <eacp/Graphics/DComp-Windows.h>
#include <eacp/Graphics/Graphics.h>
#include <eacp/Graphics/Window/CompositionHostWindow-Windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <chrono>
#include <optional>
#include <thread>

// Everything that should still work when DirectComposition does not.
//
// A session with no desktop compositor - a service, a CI runner, a job started
// over ssh into session 0 - fails DCompositionCreateDevice2 with
// E_ACCESSDENIED. Nothing can reach the screen then, but nothing in text or in
// an off-screen snapshot needs the compositor: DirectWrite measures and
// shapes, and renderToImage draws through a plain Direct2D device context. So
// those have to keep working, and the code that uses them must not crash.
//
// Its own binary because the compositor is a process-wide singleton built on
// first use: main sets EACP_DCOMP_UNAVAILABLE before anything can build it,
// which makes composition-device creation fail exactly as such a session does
// (see DComp-Windows.h), on a desktop that has a compositor as on one that has
// none.
//
// The device-loss cases share that compositor, so each leaves the devices as
// it found them.

using namespace nano;
using namespace eacp;

using Graphics::Detail::DeviceFault;

namespace
{
int argCount = 0;
char** argValues = nullptr;
int exitCode = 0;

void runTests()
{
    exitCode = nano::run(argCount, argValues);
}

Text::FontRequest uiFontRequest()
{
    auto request = Text::FontRequest {};
    request.family = "Segoe UI";
    request.pointSize = 16.f;
    request.scale = 1.f;

    return request;
}

Graphics::Font uiFont()
{
    auto options = Graphics::FontOptions {};
    options.name = "Segoe UI";
    options.size = 16.f;

    return Graphics::Font {options};
}

// White text on black, so any pixel brighter than mid-grey is ink.
struct TextSwatch final : Graphics::View
{
    TextSwatch() { setBounds({0.f, 0.f, 120.f, 40.f}); }

    void paint(Graphics::Context& g) override
    {
        g.setColor({0.f, 0.f, 0.f, 1.f});
        g.fillRect(getLocalBounds());

        g.setColor({1.f, 1.f, 1.f, 1.f});
        g.drawText("Hello", {8.f, 8.f}, font);
    }

    Graphics::Font font = uiFont();
};

int inkPixels(const Graphics::Image& image)
{
    auto count = 0;

    for (auto y = 0; y < image.height(); ++y)
        for (auto x = 0; x < image.width(); ++x)
            if (image.at(x, y).r > 0.5f)
                ++count;

    return count;
}

FilePath logPath()
{
    return FilePath::tempDirectory() / "eacp-NoCompositionTests.log";
}

bool logContains(std::string_view text)
{
    return Files::readFile(logPath()).find(text) != std::string::npos;
}

template <typename Predicate>
bool waitFor(Predicate ready)
{
    return Threads::runEventLoopUntil(ready, Time::MS {10000});
}

// Waits without pumping the event loop, as a process that never runs one does.
void sleepFor(int milliseconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds {milliseconds});
}

void clearFault(DeviceFault point)
{
    Graphics::Detail::injectDeviceFault(point, S_OK, 0);
}

// Each attempt takes one injected fault, so the ones left count the attempts.
int takeRemainingFaults(DeviceFault point)
{
    auto remaining = 0;

    while (FAILED(Graphics::Detail::takeInjectedDeviceFault(point)))
        ++remaining;

    return remaining;
}

using Clock = std::chrono::steady_clock;

std::chrono::milliseconds millisecondsSince(Clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()
                                                                 - start);
}

void restoreRenderingDevice()
{
    clearFault(DeviceFault::CreateRenderingDevice);

    if (Graphics::getD2DDevice() == nullptr)
        Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED);
}

struct BareHost
{
    BareHost()
    {
        host.hwnd = CreateWindowExW(0,
                                    L"STATIC",
                                    L"",
                                    WS_POPUP,
                                    0,
                                    0,
                                    64,
                                    64,
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
        host.transparentBackground = true;
        host.initializeComposition(true);
    }

    ~BareHost() { host.teardown(); }

    Graphics::CompositionHostWindow host;
};

// DirectComposition up for one case, where a compositor exists.
struct CompositionAllowed
{
    CompositionAllowed()
    {
        unsetEnv("EACP_DCOMP_UNAVAILABLE");
        Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED);
    }

    ~CompositionAllowed()
    {
        setEnv("EACP_DCOMP_UNAVAILABLE", "1");
        Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED);
    }
};
} // namespace

// The seam took: no composition device, and the rest of the stack up anyway.
auto tNoCompositionDevice = test("NoComposition/onlyTheCompositorIsMissing") = []
{
    check(!Graphics::isCompositorInitialized());
    check(Graphics::getCompositionDevice() == nullptr);

    check(Graphics::getDWriteFactory() != nullptr);
    check(Graphics::getD2DFactory() != nullptr);
    check(Graphics::getD2DDevice() != nullptr);
};

auto tTextMetrics = test("NoComposition/textMetricsMeasure") = []
{
    const auto font = uiFont();

    check(Graphics::TextMetrics::measureWidth("Hello", font) > 0.f);
    check(Graphics::TextMetrics::getLineHeight(font) > 0.f);
    check(Graphics::TextMetrics::getAscent(font) > 0.f);
};

auto tRasterizerShapes = test("NoComposition/rasterizerShapes") = []
{
    const auto rasterizer = Text::GlyphRasterizer(uiFontRequest());

    check(rasterizer.isValid());
    check(rasterizer.metrics({}).ascent > 0.f);

    const auto run = rasterizer.shape("Hello", {});

    check(run.glyphs.size() == 5);
    check(run.advance > 0.f);
};

auto tRenderToImage = test("NoComposition/renderToImageDrawsText") = []
{
    auto view = TextSwatch {};
    const auto image = view.renderToImage(1.f);

    check(image.isValid());
    check(image.width() == 120);
    check(image.height() == 40);
    check(inkPixels(image) > 0);
};

// Someone reading the log of a desktop run must not go looking for a session
// with no compositor: the seam says it was the seam.
auto tSeamIsLogged = test("NoComposition/theLogNamesTheSeam") = []
{
    check(Graphics::getDWriteFactory() != nullptr);
    check(logContains("EACP_DCOMP_UNAVAILABLE"));
};

auto tNonLossIgnored = test("NoComposition/aNonLossHresultIsIgnored") = []
{
    const auto generation = Graphics::getCompositionGeneration();

    check(!Graphics::handleDeviceLossIfNeeded(E_FAIL));
    check(Graphics::getCompositionGeneration() == generation);
};

auto tForcedRecovery = test("NoComposition/aForcedRecoveryKeepsEveryTier") = []
{
    auto* dwrite = Graphics::getDWriteFactory();
    auto* d2d = Graphics::getD2DFactory();
    const auto composed = Graphics::isCompositorInitialized();
    const auto generation = Graphics::getCompositionGeneration();

    check(dwrite != nullptr);
    check(d2d != nullptr);
    check(Graphics::getD2DDevice() != nullptr);

    check(Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));

    check(Graphics::getDWriteFactory() == dwrite);
    check(Graphics::getD2DFactory() == d2d);
    check(Graphics::getD2DDevice() != nullptr);
    check(Graphics::getD3DDevice() != nullptr);
    check(Graphics::getCompositionGeneration() == generation + 1);
    check(Graphics::isCompositorInitialized() == composed);
    check((Graphics::getCompositionDevice() != nullptr) == composed);

    auto view = TextSwatch {};
    check(inkPixels(view.renderToImage(1.f)) > 0);
};

// With no device nothing draws, so nothing fails into another recovery: a
// failed one has to be tried again on its own, and say why it failed.
auto tFailedRecoveryRetried = test("NoComposition/aFailedRecoveryIsRetried") = []
{
    const auto generation = Graphics::getCompositionGeneration();
    Graphics::Detail::injectDeviceFault(DeviceFault::CreateRenderingDevice,
                                        E_OUTOFMEMORY);

    check(!Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));
    check(Graphics::getD2DDevice() == nullptr);
    check(Graphics::getDWriteFactory() != nullptr);
    check(logContains("0x8007000e"));

    check(waitFor([] { return Graphics::getD2DDevice() != nullptr; }));
    check(Graphics::getCompositionGeneration() == generation + 1);

    auto view = TextSwatch {};
    check(inkPixels(view.renderToImage(1.f)) > 0);

    restoreRenderingDevice();
};

auto tRetriedUntilItWorks = test("NoComposition/aRecoveryIsRetriedUntilItWorks") = []
{
    const auto generation = Graphics::getCompositionGeneration();
    Graphics::Detail::injectDeviceFault(
        DeviceFault::CreateRenderingDevice, E_FAIL, 3);

    check(!Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_RESET));

    check(waitFor([] { return Graphics::getD2DDevice() != nullptr; }));
    check(Graphics::getCompositionGeneration() == generation + 1);
    check(
        Graphics::Detail::takeInjectedDeviceFault(DeviceFault::CreateRenderingDevice)
        == S_OK);

    restoreRenderingDevice();
};

// A compositor that was up, and did not come back with the replacement device,
// is tried again too. Needs one that can come up: none can in session 0.
auto tCompositionRetried =
    test("NoComposition/aCompositorThatFailsToComeBackIsRetried") = []
{
    auto allowed = CompositionAllowed {};

    if (!Graphics::isCompositorInitialized())
    {
        check(true);
        return;
    }

    Graphics::Detail::injectDeviceFault(DeviceFault::CreateCompositionDevice,
                                        E_FAIL);

    check(Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));
    check(!Graphics::isCompositorInitialized());

    check(waitFor([] { return Graphics::isCompositorInitialized(); }));
    check(Graphics::getCompositionDevice() != nullptr);
};

// A compositor that stays away is retried on the live rendering device, which
// is kept, backing off between attempts, and moves the generation once, when
// it comes back.
auto tCompositionRetryKeepsTheDevice =
    test("NoComposition/aCompositorRetryKeepsTheDeviceAndBacksOff") = []
{
    auto allowed = CompositionAllowed {};

    if (!Graphics::isCompositorInitialized())
    {
        check(true);
        return;
    }

    Graphics::Detail::injectDeviceFault(
        DeviceFault::CreateCompositionDevice, E_FAIL, 3);
    check(Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));

    const auto generation = Graphics::getCompositionGeneration();
    const auto device = Graphics::ComPtr<ID3D11Device> {Graphics::getD3DDevice()};
    const auto start = std::chrono::steady_clock::now();

    check(waitFor([] { return Graphics::isCompositorInitialized(); }));

    const auto elapsed = std::chrono::steady_clock::now() - start;

    check(Graphics::getD3DDevice() == device.Get());
    check(Graphics::getCompositionGeneration() == generation + 1);
    check(elapsed >= std::chrono::milliseconds {3000});
};

// A process that never pumps the event loop still gets its device back: a
// snapshot that finds none runs the next attempt itself once it is due, and
// not before.
auto tSnapshotRetriesWithoutTheLoop =
    test("NoComposition/aSnapshotRetriesAFailedRecoveryWithoutTheLoop") = []
{
    auto view = TextSwatch {};
    Graphics::Detail::injectDeviceFault(
        DeviceFault::CreateRenderingDevice, E_FAIL, 2);

    check(!Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));
    check(!view.renderToImage(1.f).isValid());

    sleepFor(700);
    check(!view.renderToImage(1.f).isValid());
    check(!view.renderToImage(1.f).isValid());

    sleepFor(1200);
    check(inkPixels(view.renderToImage(1.f)) > 0);

    restoreRenderingDevice();
};

// A hosted copy detaching drops the scheduled retry unrun; that must not stop
// the next one from being scheduled.
auto tDroppedRetryIsRescheduled =
    test("NoComposition/aRetryTheSchedulerDroppedIsScheduledAgain") = []
{
    Threads::runEventLoopFor(Time::MS {50});

    auto view = TextSwatch {};
    Graphics::Detail::injectDeviceFault(
        DeviceFault::CreateRenderingDevice, E_FAIL, 2);

    check(!Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));
    Threads::stopCallAfterScheduler();

    sleepFor(700);
    check(!view.renderToImage(1.f).isValid());

    check(waitFor([] { return Graphics::getD2DDevice() != nullptr; }));

    restoreRenderingDevice();
};

// A hosted copy that detaches and attaches again drops the retry it had
// scheduled, and the first thing to need the devices after is the host it
// opens, often with no snapshot to retry from: the host restarts the retries.
// Here the retry is dropped before it is due, still with the scheduler.
auto tRetryDroppedBeforeItIsDueResumedByAHost =
    test("NoComposition/aRetryADetachDroppedBeforeItWasDueIsResumedByANewHost") = []
{
    check(Graphics::getDWriteFactory() != nullptr);
    Threads::runEventLoopFor(Time::MS {50});

    Graphics::Detail::injectDeviceFault(
        DeviceFault::CreateRenderingDevice, E_FAIL, 2);
    check(!Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));

    Threads::detachCurrentThreadAsMain();
    Threads::attachCurrentThreadAsMain();

    auto bare = BareHost {};
    check(waitFor([] { return Graphics::getD2DDevice() != nullptr; }));

    restoreRenderingDevice();
};

// The same, dropped once due: handed to the event loop, and not yet run.
auto tRetryDroppedOnceDueResumedByAHost =
    test("NoComposition/aRetryADetachDroppedOnceDueIsResumedByANewHost") = []
{
    check(Graphics::getDWriteFactory() != nullptr);
    Threads::runEventLoopFor(Time::MS {50});

    Graphics::Detail::injectDeviceFault(
        DeviceFault::CreateRenderingDevice, E_FAIL, 2);
    check(!Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));
    sleepFor(700);

    Threads::detachCurrentThreadAsMain();
    Threads::attachCurrentThreadAsMain();

    auto bare = BareHost {};
    check(waitFor([] { return Graphics::getD2DDevice() != nullptr; }));

    restoreRenderingDevice();
};

// Retries are scheduled on the event loop, not on a host window, so closing a
// host drops none.
auto tRetryOutlivesAClosedHost =
    test("NoComposition/aRetryOutlivesAHostClosedWhileItIsPending") = []
{
    check(Graphics::getDWriteFactory() != nullptr);
    Threads::runEventLoopFor(Time::MS {50});

    auto bare = std::optional<BareHost> {std::in_place};
    Graphics::Detail::injectDeviceFault(
        DeviceFault::CreateRenderingDevice, E_FAIL, 2);
    check(!Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));

    bare.reset();
    check(waitFor([] { return Graphics::getD2DDevice() != nullptr; }));

    restoreRenderingDevice();
};

// A host opened while the scheduled retry is still live takes its place
// rather than running a second chain of attempts beside it. Counted before the
// second retry: an extra attempt at 0.5 s doubles the backoff again, and by
// the third the attempt it pushed out would hide it.
auto tHostOpenedMidRetryStartsNoSecondChain =
    test("NoComposition/aHostOpenedDuringARetryStartsNoSecondChain") = []
{
    check(Graphics::getDWriteFactory() != nullptr);
    Threads::runEventLoopFor(Time::MS {50});

    constexpr auto faults = 100;
    Graphics::Detail::injectDeviceFault(
        DeviceFault::CreateRenderingDevice, E_FAIL, faults);
    check(!Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));

    auto bare = BareHost {};

    // The loss, then the attempt at 0.5 s; the next is at 1.5 s. A second chain
    // makes one more at 0.5 s.
    Threads::runEventLoopFor(Time::MS {1100});

    const auto attempts =
        faults - takeRemainingFaults(DeviceFault::CreateRenderingDevice);
    check(attempts == 2);

    restoreRenderingDevice();
};

// A compositor retry dropped by a detach, and due by the time a new host opens:
// the host runs it there and then, and the compositor coming back rebuilds the
// hosts already open before the new one builds on it. Needs a compositor that
// can come up: none can in session 0.
auto tDueCompositorRetryRunByANewHost =
    test("NoComposition/aDueCompositorRetryRunByANewHostRebuildsTheOthers") = []
{
    auto allowed = CompositionAllowed {};

    if (!Graphics::isCompositorInitialized())
    {
        check(true);
        return;
    }

    auto open = BareHost {};
    check(open.host.target != nullptr);

    Graphics::Detail::injectDeviceFault(DeviceFault::CreateCompositionDevice,
                                        E_FAIL);
    check(Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));
    check(!Graphics::isCompositorInitialized());
    check(open.host.target == nullptr);

    Threads::detachCurrentThreadAsMain();
    Threads::attachCurrentThreadAsMain();
    sleepFor(700);

    auto opened = BareHost {};
    check(Graphics::isCompositorInitialized());
    check(open.host.target != nullptr);
    check(opened.host.target != nullptr);
    check(open.host.generation == Graphics::getCompositionGeneration());
    check(opened.host.generation == Graphics::getCompositionGeneration());
};

// A snapshot that runs the due attempt while the scheduled callback for it
// waits unpumped leaves one chain of attempts: the late callback must not run
// a second beside the snapshot's. The compositor is built first, so its
// constructor takes none of the faults.
auto tLateRetryStartsNoSecondChain =
    test("NoComposition/aLateRetryBesideASnapshotRetryStartsNoSecondChain") = []
{
    check(Graphics::getDWriteFactory() != nullptr);
    Threads::runEventLoopFor(Time::MS {50});

    constexpr auto faults = 100;
    auto view = TextSwatch {};
    Graphics::Detail::injectDeviceFault(
        DeviceFault::CreateRenderingDevice, E_FAIL, faults);

    check(!Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));
    sleepFor(700);
    check(!view.renderToImage(1.f).isValid());

    // One chain: the loss, the snapshot's attempt, one a second later, and the
    // next two seconds after that, outside the wait. Two chains make a fourth
    // at once.
    Threads::runEventLoopFor(Time::MS {2000});

    const auto attempts =
        faults - takeRemainingFaults(DeviceFault::CreateRenderingDevice);
    check(attempts == 3);

    restoreRenderingDevice();
};

// A recovery that came some other way - another loss signal - leaves the last
// backed-off callback pending. A new loss is retried from the first delay,
// not when that callback comes round.
auto tNewLossRetriedFromTheFirstDelay =
    test("NoComposition/aNewLossAfterAnOutOfBandRecoveryIsRetriedPromptly") = []
{
    check(Graphics::getDWriteFactory() != nullptr);
    Threads::runEventLoopFor(Time::MS {50});

    // Attempts at 0, 0.5, 1.5, 3.5 and 7.5 s leave the next one pending at
    // 15.5 s, far enough out that a loaded machine's delays stay inside the
    // bound and a retry that waited for it stays outside.
    Graphics::Detail::injectDeviceFault(
        DeviceFault::CreateRenderingDevice, E_FAIL, 100);
    check(!Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));
    Threads::runEventLoopFor(Time::MS {8000});
    check(Graphics::getD2DDevice() == nullptr);

    clearFault(DeviceFault::CreateRenderingDevice);
    check(Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));

    Graphics::Detail::injectDeviceFault(DeviceFault::CreateRenderingDevice, E_FAIL);
    const auto start = Clock::now();

    check(!Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));
    check(waitFor([] { return Graphics::getD2DDevice() != nullptr; }));
    check(millisecondsSince(start) < std::chrono::milliseconds {3000});

    restoreRenderingDevice();
};

// A compositor retry that finds composition turned off settles for none, as a
// full recovery does, rather than retrying for as long as the process runs.
auto tCompositionRetryHonoursTheSeam =
    test("NoComposition/aCompositorRetryThatFindsTheSeamStops") = []
{
    auto allowed = CompositionAllowed {};

    if (!Graphics::isCompositorInitialized())
    {
        check(true);
        return;
    }

    Graphics::Detail::injectDeviceFault(
        DeviceFault::CreateCompositionDevice, E_FAIL, 100);
    check(Graphics::handleDeviceLossIfNeeded(DXGI_ERROR_DEVICE_REMOVED));
    check(!Graphics::isCompositorInitialized());

    // The retry at 0.5 s finds the seam.
    setEnv("EACP_DCOMP_UNAVAILABLE", "1");
    Threads::runEventLoopFor(Time::MS {1300});

    // A retry still scheduled (1.5 or 3.5 s) would bring the compositor back.
    unsetEnv("EACP_DCOMP_UNAVAILABLE");
    clearFault(DeviceFault::CreateCompositionDevice);
    Threads::runEventLoopFor(Time::MS {4000});

    check(!Graphics::isCompositorInitialized());
};

// A snapshot is often the only drawing a headless process does, so a device
// it finds lost has to start the recovery itself. That snapshot is lost; the
// next one draws.
auto tEndDrawLoss =
    test("NoComposition/aSnapshotWhoseEndDrawFindsTheDeviceLostRecoversIt") = []
{
    auto view = TextSwatch {};
    const auto generation = Graphics::getCompositionGeneration();
    Graphics::Detail::injectDeviceFault(DeviceFault::SnapshotEndDraw,
                                        D2DERR_RECREATE_TARGET);

    check(!view.renderToImage(1.f).isValid());
    check(Graphics::getCompositionGeneration() == generation + 1);
    check(inkPixels(view.renderToImage(1.f)) > 0);
};

auto tContextLoss =
    test("NoComposition/aSnapshotWhoseContextFindsTheDeviceLostRecoversIt") = []
{
    auto view = TextSwatch {};
    const auto generation = Graphics::getCompositionGeneration();
    Graphics::Detail::injectDeviceFault(DeviceFault::SnapshotCreateDeviceContext,
                                        DXGI_ERROR_DEVICE_REMOVED);

    check(!view.renderToImage(1.f).isValid());
    check(Graphics::getCompositionGeneration() == generation + 1);
    check(inkPixels(view.renderToImage(1.f)) > 0);
};

auto tSnapshotNonLoss =
    test("NoComposition/aSnapshotFailureThatIsNoLossRecoversNothing") = []
{
    auto view = TextSwatch {};
    const auto generation = Graphics::getCompositionGeneration();
    Graphics::Detail::injectDeviceFault(DeviceFault::SnapshotEndDraw, E_INVALIDARG);

    check(!view.renderToImage(1.f).isValid());
    check(Graphics::getCompositionGeneration() == generation);
    check(inkPixels(view.renderToImage(1.f)) > 0);
};

// What the host registry answers - transparency, the cursor refresh - needs no
// compositor, so a host joins it whether or not there is one.
auto tHostRegistered = test("NoComposition/aHostWithNoCompositorIsRegistered") = []
{
    auto bare = BareHost {};

    check(bare.host.hwnd != nullptr);
    check(bare.host.target == nullptr);
    check(Graphics::isHostWindowTransparent(bare.host.hwnd));
};

// And a registered host is the one a recovery rebuilds, so a compositor that
// comes back reaches the hosts opened while it was gone.
auto tHostRebuilt =
    test("NoComposition/aRecoveryThatBringsTheCompositorBackBuildsTheHost") = []
{
    auto bare = BareHost {};
    check(bare.host.target == nullptr);

    auto allowed = CompositionAllowed {};

    if (!Graphics::isCompositorInitialized())
    {
        check(bare.host.target == nullptr);
        return;
    }

    check(bare.host.target != nullptr);
    check(bare.host.rootVisual != nullptr);
    check(bare.host.generation == Graphics::getCompositionGeneration());
};

int main(int argc, char* argv[])
{
    argCount = argc;
    argValues = argv;

    Files::removeFile(logPath());
    setLogFile(logPath().str());
    setEnv("EACP_DCOMP_UNAVAILABLE", "1");

    Apps::getAppEnvironment().headless = true;
    Apps::run(runTests);

    return exitCode;
}
