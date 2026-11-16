#include "Common.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Logging.h>
#include <eacp/Graphics/Window/Window.h>

#include <mach/mach.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// An app that keeps many GPUViews alive and shows one at a time (a tab per
// document, a terminal per session) swaps them in and out of a window. A
// CAMetalLayer that has been on screen keeps its drawables after it leaves the
// window, so without hiding giving them back every view ever shown holds its
// full-size buffers for as long as it lives. Measured as the process's
// physical footprint, which is where those IOSurfaces are charged.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto viewCount = 6;
constexpr auto framesPerView = 3;
constexpr auto viewWidth = 1024.f;
constexpr auto viewHeight = 768.f;
constexpr auto releaseTimeout = Time::MS {5000};
constexpr auto pollInterval = Time::MS {50};
constexpr auto steadyInterval = Time::MS {500};
constexpr auto bytesPerMegabyte = std::int64_t {1024 * 1024};

// Below this the views got no real drawables, so nothing is measured.
constexpr auto smallestMeaningfulGrowth = std::int64_t {64} * bytesPerMegabyte;

// What hiding may keep of the frames' memory: driver caches, the pipelines.
constexpr auto largestShareKeptAfterHiding = 0.1;

std::int64_t getPhysicalFootprint()
{
    auto info = task_vm_info_data_t {};
    auto count = mach_msg_type_number_t {TASK_VM_INFO_COUNT};
    const auto result =
        task_info(mach_task_self(), TASK_VM_INFO, (task_info_t) &info, &count);

    if (result != KERN_SUCCESS)
        return 0;

    return (std::int64_t) info.phys_footprint;
}

// Rendering leaves transient memory that drains for a moment; measuring from
// before it settles would credit that to whatever happens next.
std::int64_t waitForSteadyFootprint()
{
    auto deadline = Time::Deadline {releaseTimeout};
    auto previous = getPhysicalFootprint();

    while (!deadline.expired())
    {
        Threads::runEventLoopFor(steadyInterval);
        const auto current = getPhysicalFootprint();

        if (current >= previous - bytesPerMegabyte)
            return current;

        previous = current;
    }

    return previous;
}

std::int64_t waitForFootprintAtMost(std::int64_t limit)
{
    auto deadline = Time::Deadline {releaseTimeout};

    while (getPhysicalFootprint() > limit && !deadline.expired())
        Threads::runEventLoopFor(pollInterval);

    return getPhysicalFootprint();
}

std::int64_t toMegabytes(std::int64_t bytes)
{
    return bytes / bytesPerMegabyte;
}

struct ClearingView final : GPUView
{
    ClearingView() { setDepth(true); }

    void render(Frame& frame) override { auto pass = frame.beginPass({}); }

    bool renderOneFrame()
    {
        return renderFrame([this](Frame& frame) { render(frame); });
    }
};

Graphics::WindowOptions makeHiddenViewTestWindowOptions()
{
    auto options = Graphics::WindowOptions {};
    options.title = "eacp hidden view memory tests";
    options.width = (int) viewWidth;
    options.height = (int) viewHeight;
    return options;
}

// Puts the view in the window in place of the one before it and renders it.
bool showInPlaceOf(Graphics::View& host, ClearingView& shown, ClearingView* previous)
{
    if (previous != nullptr)
        host.removeSubview(*previous);

    host.addSubview(shown);
    shown.setBounds({0.f, 0.f, viewWidth, viewHeight});
    shown.resizeStarted();

    for (auto frame = 0; frame < framesPerView; ++frame)
        if (!shown.renderOneFrame())
            return false;

    return true;
}
} // namespace

auto tHiddenViewsReleaseFrameMemory =
    test("HiddenViewMemory/hidingAViewReleasesItsDrawablesAndTargets") = []
{
    if (!Device::shared().isValid() || Apps::getAppEnvironment().headless)
        return;

    auto host = Graphics::View {};
    auto window = Graphics::Window {makeHiddenViewTestWindowOptions()};
    window.setContentView(host);
    window.setVisible(true);

    auto views = std::vector<std::unique_ptr<ClearingView>> {};

    for (auto index = 0; index < viewCount; ++index)
        views.push_back(std::make_unique<ClearingView>());

    const auto beforeRendering = waitForSteadyFootprint();
    auto* previous = (ClearingView*) nullptr;

    for (auto& view: views)
    {
        check(showInPlaceOf(host, *view, previous), "a view in a window gets drawables");
        previous = view.get();
    }

    const auto growth = waitForSteadyFootprint() - beforeRendering;

    if (growth < smallestMeaningfulGrowth)
    {
        LOG("HiddenViewMemory: skipped, rendering grew the footprint by only "
            + std::to_string(toMegabytes(growth)) + " MB");
        return;
    }

    for (auto& view: views)
        view->setVisible(false);

    const auto allowedToKeep =
        (std::int64_t) ((double) growth * largestShareKeptAfterHiding);
    const auto keptAfterHiding =
        waitForFootprintAtMost(beforeRendering + allowedToKeep) - beforeRendering;

    LOG("HiddenViewMemory: rendering added " + std::to_string(toMegabytes(growth))
        + " MB, hiding kept " + std::to_string(toMegabytes(keptAfterHiding)) + " MB");

    check(keptAfterHiding <= allowedToKeep,
          "hidden views keep no drawables, multisample or depth textures");

    for (auto& view: views)
        view->setVisible(true);

    check(previous->renderOneFrame(), "a view shown again renders as before");
};
