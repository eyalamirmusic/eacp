#include "Common.h"

#include <eacp/Core/Threads/EventLoop.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/Graphics/View/View-Linux.h>

#include <wayland-client.h>

#include <cstring>
#include <optional>
#include <sys/mman.h>
#include <unistd.h>

// The Wayland backend against a real compositor: not headless, and every case
// self-skips without one, unless EACP_REQUIRE_DISPLAY=1. Input is not covered.

using namespace nano;
using namespace eacp;
using namespace eacp::Graphics;

namespace
{
constexpr auto waylandTestTimeout = Time::MS {5000};

// The size Scripts/with-weston starts Weston's headless backend at.
constexpr float waylandTestOutputWidth = 1280.f;
constexpr float waylandTestOutputHeight = 800.f;

// Decided by trying: the backend degrades to headless silently.
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

// The view is declared first so it outlives the window.
struct CompositorWindow
{
    explicit CompositorWindow(int width = 640, int height = 400)
    {
        auto options = WindowOptions {};
        options.width = width;
        options.height = height;
        options.title = "eacp Wayland tests";

        // A close here must not take the test runner down with it.
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
        // ~View fires onLost when this object's own members are already gone.
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

// A frame callback only arrives once the surface is committed with a buffer.
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

// The one case that does not self-skip.
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

    // A compositor with no opinion lets the client's own size stand.
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

    // The contract is that scale and pixel size agree, not their values.
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

    check(presenter.lost == 0);
    check(presenter.available == 1);

    // A move with no size change is not a resize.
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

    // Effective visibility is the whole ancestor chain.
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

    // onLost must reach the presenter while the wl_surface is still alive.
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

    // Wayland publishes no work area, so it is the whole display.
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

    // The request first, the commit second: wl_surface.frame rides on the next
    // commit, which for this surface only a presenter ever makes.
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

// Apps::run wraps it: runEventLoopUntil needs a bootstrapped loop.
int main(int argc, char* argv[])
{
    waylandTestArgCount = argc;
    waylandTestArgValues = argv;

    eacp::Apps::run(runWaylandTests);

    return waylandTestExitCode;
}
