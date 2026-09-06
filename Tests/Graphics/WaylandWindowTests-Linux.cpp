#include "Common.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/Graphics/View/View-Linux.h>

#include <wayland-client.h>

#include <cstring>
#include <optional>
#include <sys/mman.h>
#include <unistd.h>

// The Wayland backend against a real compositor.
//
// Everything else in this directory runs headless on purpose - GraphicsTests
// forces Apps::getAppEnvironment().headless before nano::run - because the
// model under test is portable and a window on screen adds nothing to it. This
// binary is the opposite case: the only thing it checks is what the compositor
// does, so it has a main of its own that leaves headless alone and every case
// self-skips when there is no compositor to talk to.
//
// EACP_REQUIRE_DISPLAY=1 turns that skip into a failure, the way
// EACP_REQUIRE_GPU=1 does for the Vulkan suite, so a CI lane that meant to run
// under Weston and did not cannot report a green run of nothing.
//
// What is NOT covered here, and cannot be: input. A headless Weston advertises
// no wl_seat at all - no pointer, no keyboard, not even empty ones - so nothing
// in WaylandInput-Linux.cpp is reachable from this process. The parts of that
// file that can be tested without a seat are tested without Wayland:
// KeyCodeTests-Linux.cpp covers the evdev table, and the routing it performs is
// View.cpp's own hit-tester, which ScrollWheelTests and ViewWindowTests cover
// from the portable side.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
constexpr auto waylandTestTimeout = Time::MS {5000};

// Weston's headless backend is started at this size by the with-weston wrapper
// in the Dockerfile, and it is also the size Display.h falls back to when there
// is no compositor at all - so the display case below checks the reported
// numbers against the output rather than against a constant, and only asserts
// the shape.
constexpr float waylandTestOutputWidth = 1280.f;
constexpr float waylandTestOutputHeight = 800.f;

// Whether a window can actually be put on screen, decided once by trying.
// Cheaper answers - WAYLAND_DISPLAY being set, the socket existing - are
// necessary and not sufficient: the compositor may refuse the connection, and
// the backend degrades to headless silently when it does, which is exactly the
// state that must not read as a pass.
bool waylandCompositorReachable()
{
    static const auto reachable = []
    {
        if (Apps::getAppEnvironment().headless)
            return false;

        if (getEnvValue("WAYLAND_DISPLAY").empty()
            && getEnvValue("WAYLAND_SOCKET").empty())
            return false;

        auto probe = Window {WindowOptions {}};

        return probe.getHandle() != nullptr;
    }();

    return reachable;
}

// A window with a content view, mapped and configured, or nothing at all.
//
// The view is declared first so it outlives the window: ~Window tears the
// subsurfaces down through the content view, and one case here destroys the
// window on purpose while the view watches.
struct CompositorWindow
{
    explicit CompositorWindow(int width = 640, int height = 400)
    {
        auto options = WindowOptions {};
        options.width = width;
        options.height = height;
        options.title = "eacp Wayland tests";

        // Not the primary window: a close here must not take the test runner
        // down with it.
        options.isPrimary = false;

        window.emplace(options);
        window->setContentView(content);

        Threads::runEventLoopUntil([this] { return window->isVisible(); },
                                   waylandTestTimeout);
    }

    bool isUp() { return window.has_value() && window->isVisible(); }

    View content;
    std::optional<Window> window;
};

// A view that presents its own pixels, which is what a GPUView is and the only
// kind of view Linux gives a surface to. The counters are what the ViewSurface
// contract promises, counted.
struct PresentingView : View
{
    PresentingView()
        : record(requestViewSurface(*this))
    {
        record.onAvailable = [this] { ++available; };
        record.onLost = [this] { ++lost; };
        record.onResized = [this] { ++resized; };
        record.onFrameDone = [this] { ++frames; };
    }

    ~PresentingView() override
    {
        // ~View fires onLost while this object's own members are already gone,
        // so the hooks are dropped on the way past.
        record.onAvailable = [] {};
        record.onLost = [] {};
        record.onResized = [] {};
        record.onFrameDone = [] {};
    }

    bool hasSurface() const { return record.surface != nullptr; }

    ViewSurface& record;
    int available = 0;
    int lost = 0;
    int resized = 0;
    int frames = 0;
};

// The test's own wl_shm, bound off a registry of its own on the connection the
// backend already owns. Needed for exactly one case: a frame callback only
// arrives after the surface it was requested on is committed with a buffer, and
// the buffer is the presenter's job - which here is us.
wl_shm* waylandTestShm(wl_display* display)
{
    static wl_shm* cached = nullptr;

    if (cached != nullptr || display == nullptr)
        return cached;

    static const wl_registry_listener listener {
        .global =
            [](void* data,
               wl_registry* registry,
               uint32_t name,
               const char* interface,
               uint32_t)
        {
            if (std::strcmp(interface, wl_shm_interface.name) == 0)
                *static_cast<wl_shm**>(data) = static_cast<wl_shm*>(
                    wl_registry_bind(registry, name, &wl_shm_interface, 1));
        },
        .global_remove = [](void*, wl_registry*, uint32_t) {},
    };

    auto* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &listener, &cached);
    wl_display_roundtrip(display);
    wl_registry_destroy(registry);

    return cached;
}

struct TestShmBuffer
{
    ~TestShmBuffer()
    {
        if (buffer != nullptr)
            wl_buffer_destroy(buffer);

        if (pixels != nullptr)
            ::munmap(pixels, byteSize);
    }

    bool create(wl_shm* shm, int width, int height)
    {
        if (shm == nullptr || width <= 0 || height <= 0)
            return false;

        const auto stride = width * 4;
        byteSize = (size_t) stride * (size_t) height;

        auto fd = ::memfd_create("eacp-wayland-test", MFD_CLOEXEC);

        if (fd < 0 || ::ftruncate(fd, (off_t) byteSize) != 0)
        {
            if (fd >= 0)
                ::close(fd);

            return false;
        }

        pixels =
            ::mmap(nullptr, byteSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

        if (pixels == MAP_FAILED)
        {
            pixels = nullptr;
            ::close(fd);
            return false;
        }

        auto* pool = wl_shm_create_pool(shm, fd, (int32_t) byteSize);
        buffer = wl_shm_pool_create_buffer(
            pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        ::close(fd);

        return buffer != nullptr;
    }

    wl_buffer* buffer = nullptr;
    void* pixels = nullptr;
    size_t byteSize = 0;
};
} // namespace

// The one case that does not self-skip, and the answer to the failure mode
// every other one has: a case whose compositor is missing returns immediately
// and ctest scores it as a pass, so a lane whose Weston never came up reports a
// full green suite that opened no windows at all.
auto tCompositorIsPresentWhenRequired =
    test("Wayland/aCompositorIsPresentWhenRequired") = []
{
    const auto reachable = waylandCompositorReachable();

    LOG("Wayland compositor: ",
        reachable ? getEnvValue("WAYLAND_DISPLAY") : std::string {"none reached"});

    if (getEnvValue("EACP_REQUIRE_DISPLAY") != "1")
        return;

    check(reachable,
          "EACP_REQUIRE_DISPLAY=1 but no Wayland compositor was reached - every "
          "other case in this binary would have skipped and reported a pass");
};

auto tWindowComesUpAtItsSize = test("Wayland/windowComesUpAtItsConfiguredSize") = []
{
    if (!waylandCompositorReachable())
        return;

    auto host = CompositorWindow {640, 400};

    check(host.isUp());

    // A floating toplevel is configured with no size by every compositor that
    // has no opinion, which means the client's own size stands.
    const auto bounds = host.content.getBounds();

    check(bounds.w == 640.f);
    check(bounds.h == 400.f);
};

auto tWindowHandleIsTheSurface = test("Wayland/windowHandleIsTheSurface") = []
{
    if (!waylandCompositorReachable())
        return;

    auto host = CompositorWindow {};

    check(host.window->getHandle() != nullptr);

    // The content view's own identity, which is a different thing and must not
    // be the same pointer - a caller told they were the same would hand a
    // View::Native to vkCreateWaylandSurfaceKHR.
    check(host.window->getContentViewHandle() != host.window->getHandle());
};

auto tViewSurfaceBecomesAvailable = test("Wayland/viewSurfaceBecomesAvailable") = []
{
    if (!waylandCompositorReachable())
        return;

    auto host = CompositorWindow {640, 400};
    auto presenter = PresentingView {};

    presenter.setBounds({10.f, 20.f, 320.f, 240.f});
    host.content.addSubview(presenter);

    Threads::runEventLoopUntil([&] { return presenter.available > 0; },
                               waylandTestTimeout);

    check(presenter.available == 1);
    check(presenter.lost == 0);
    check(presenter.record.display != nullptr);
    check(presenter.record.surface != nullptr);

    // The scale the compositor asked for, and the pixel size that follows from
    // it. Not asserted as 1: a compositor is entitled to say otherwise, and the
    // contract is that the two agree, not that either is a particular number.
    check(presenter.record.scale > 0.f);
    check(presenter.record.pixelWidth
          == (int) std::lround(320.f * presenter.record.scale));
    check(presenter.record.pixelHeight
          == (int) std::lround(240.f * presenter.record.scale));
};

auto tMovingTheViewResizesItsSurface =
    test("Wayland/movingTheViewResizesItsSurface") = []
{
    if (!waylandCompositorReachable())
        return;

    auto host = CompositorWindow {640, 400};
    auto presenter = PresentingView {};

    presenter.setBounds({0.f, 0.f, 320.f, 240.f});
    host.content.addSubview(presenter);

    Threads::runEventLoopUntil([&] { return presenter.available > 0; },
                               waylandTestTimeout);
    check(presenter.available == 1);

    const auto scale = presenter.record.scale;
    const auto resizesBefore = presenter.resized;

    presenter.setBounds({40.f, 60.f, 160.f, 120.f});

    check(presenter.resized == resizesBefore + 1);
    check(presenter.record.pixelWidth == (int) std::lround(160.f * scale));
    check(presenter.record.pixelHeight == (int) std::lround(120.f * scale));

    // The surface itself survives a resize: the swapchain is rebuilt against
    // it, not replaced with it.
    check(presenter.lost == 0);
    check(presenter.available == 1);

    // A move with no size change is not a resize, whatever it does to the
    // subsurface's position.
    const auto resizesAfterMove = presenter.resized;
    presenter.setBounds({80.f, 90.f, 160.f, 120.f});
    check(presenter.resized == resizesAfterMove);
};

auto tHidingTheViewTakesItsSurfaceAway =
    test("Wayland/hidingTheViewTakesItsSurfaceAway") = []
{
    if (!waylandCompositorReachable())
        return;

    auto host = CompositorWindow {640, 400};
    auto presenter = PresentingView {};

    presenter.setBounds({0.f, 0.f, 200.f, 100.f});
    host.content.addSubview(presenter);

    Threads::runEventLoopUntil([&] { return presenter.available > 0; },
                               waylandTestTimeout);
    check(presenter.available == 1);

    presenter.setVisible(false);

    check(presenter.lost == 1);
    check(!presenter.hasSurface());
    check(presenter.record.display == nullptr);
    check(presenter.record.pixelWidth == 0);
    check(presenter.record.pixelHeight == 0);
    check(!presenter.record.frameCallbackPending);

    presenter.setVisible(true);

    check(presenter.available == 2);
    check(presenter.hasSurface());

    // Hiding the CONTAINER is the same news, since effective visibility is the
    // whole ancestor chain.
    host.content.setVisible(false);

    check(presenter.lost == 2);
    check(!presenter.hasSurface());
};

auto tDestroyingTheWindowTakesTheSurfaceAway =
    test("Wayland/destroyingTheWindowTakesTheSurfaceAway") = []
{
    if (!waylandCompositorReachable())
        return;

    auto host = CompositorWindow {640, 400};
    auto presenter = PresentingView {};

    presenter.setBounds({0.f, 0.f, 200.f, 100.f});
    host.content.addSubview(presenter);

    Threads::runEventLoopUntil([&] { return presenter.available > 0; },
                               waylandTestTimeout);
    check(presenter.available == 1);

    // onLost has to reach the presenter while the wl_surface is still alive: a
    // swapchain outliving the surface it was made from is a use-after-free
    // inside the driver, and this is the ordering that prevents it.
    host.window.reset();

    check(presenter.lost == 1);
    check(!presenter.hasSurface());
    check(presenter.record.surface == nullptr);
};

auto tPrimaryDisplayReportsTheOutput =
    test("Wayland/primaryDisplayReportsTheCompositorsOutput") = []
{
    if (!waylandCompositorReachable())
        return;

    const auto display = primaryDisplay();

    check(display.frame.w == waylandTestOutputWidth);
    check(display.frame.h == waylandTestOutputHeight);
    check(display.backingScale >= 1.f);

    // Wayland publishes no work area - a panel is an ordinary client - so the
    // whole display is what an app is told it may use.
    check(display.workArea.w == display.frame.w);
    check(display.workArea.h == display.frame.h);
};

auto tFrameCallbackArrivesAfterACommit =
    test("Wayland/frameCallbackArrivesAfterACommit") = []
{
    if (!waylandCompositorReachable())
        return;

    auto host = CompositorWindow {640, 400};
    auto presenter = PresentingView {};

    presenter.setBounds({0.f, 0.f, 320.f, 240.f});
    host.content.addSubview(presenter);

    Threads::runEventLoopUntil([&] { return presenter.available > 0; },
                               waylandTestTimeout);
    check(presenter.available == 1);

    auto* shm = waylandTestShm(presenter.record.display);
    check(shm != nullptr);

    auto buffer = TestShmBuffer {};
    check(buffer.create(
        shm, presenter.record.pixelWidth, presenter.record.pixelHeight));

    // The request first, the commit second: wl_surface.frame is queued on the
    // surface and carried by its NEXT commit, which on a real presenter is the
    // swapchain's present. Standing in for that here is the whole point of the
    // case - nothing in the backend commits this surface.
    presenter.record.requestFrameCallback();
    check(presenter.record.frameCallbackPending);

    wl_surface_attach(presenter.record.surface, buffer.buffer, 0, 0);
    wl_surface_damage_buffer(presenter.record.surface,
                             0,
                             0,
                             presenter.record.pixelWidth,
                             presenter.record.pixelHeight);
    wl_surface_commit(presenter.record.surface);
    wl_display_flush(presenter.record.display);

    Threads::runEventLoopUntil([&] { return presenter.frames > 0; },
                               waylandTestTimeout);

    check(presenter.frames == 1);
    check(!presenter.record.frameCallbackPending);
};

namespace
{
int waylandTestArgCount = 0;
char** waylandTestArgValues = nullptr;
int waylandTestExitCode = 0;

void runWaylandTests()
{
    waylandTestExitCode = nano::run(waylandTestArgCount, waylandTestArgValues);
}
} // namespace

// Deliberately unlike Tests/Graphics/TestMain.cpp, which forces headless: this
// binary is the one place that wants a real window. Apps::run is still what
// wraps it, because a window needs the environment initLoopThread() sets up and
// because runEventLoopUntil only works inside a bootstrapped loop.
int main(int argc, char* argv[])
{
    waylandTestArgCount = argc;
    waylandTestArgValues = argv;

    eacp::Apps::run(runWaylandTests);

    return waylandTestExitCode;
}
