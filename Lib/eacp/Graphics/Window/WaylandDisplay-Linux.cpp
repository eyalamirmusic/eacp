#include "WaylandDisplay-Linux.h"

#include "WaylandClipboard-Linux.h"
#include "WaylandInput-Linux.h"

#include <eacp/Core/App/AppEnvironment.h>
#include <eacp/Core/Threads/EventLoop-Linux.h>
#include <eacp/Core/Utils/Environment.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

// Reads through prepare_read / read_events, not wl_display_dispatch: Mesa's
// Vulkan WSI is a second reader of this same wl_display.

namespace eacp::Graphics
{
namespace
{
// Bound at min(this, what the compositor offered).
constexpr uint32_t waylandCompositorVersion = 6;
constexpr uint32_t waylandSeatVersion = 8;
constexpr uint32_t waylandOutputVersion = 4;
constexpr uint32_t waylandXdgShellVersion = 5;
constexpr uint32_t waylandXdgOutputVersion = 3;
constexpr uint32_t waylandDataDeviceVersion = 3;

// wl_output.mode reports millihertz, and zero for an unknown rate.
constexpr int waylandDefaultRefreshMilliHz = 60'000;

template <typename T>
T* waylandBind(wl_registry* registry,
               uint32_t name,
               const wl_interface* interface,
               uint32_t offered,
               uint32_t wanted)
{
    auto version = std::min(offered, wanted);

    return static_cast<T*>(wl_registry_bind(registry, name, interface, version));
}

uint32_t waylandPremultipliedPixel(Color colour)
{
    auto channel = [](float value)
    {
        auto scaled = (int) std::lround(std::clamp(value, 0.f, 1.f) * 255.f);
        return (uint32_t) scaled;
    };

    // WL_SHM_FORMAT_ARGB8888 is premultiplied little-endian: 0xAARRGGBB.
    return (channel(colour.a) << 24) | (channel(colour.r * colour.a) << 16)
           | (channel(colour.g * colour.a) << 8) | channel(colour.b * colour.a);
}

bool waylandEnvironmentNamesACompositor()
{
    return !getEnvValue("WAYLAND_DISPLAY").empty()
           || !getEnvValue("WAYLAND_SOCKET").empty();
}
} // namespace

Point WaylandOutputInfo::logicalSize() const
{
    if (hasXdgLogicalSize)
        return xdgLogicalSize;

    auto divisor = scale > 0 ? (float) scale : 1.f;

    return {modeSize.x / divisor, modeSize.y / divisor};
}

struct WaylandRegistryDispatch
{
    static WaylandDisplay& self(void* data)
    {
        return *static_cast<WaylandDisplay*>(data);
    }

    static WaylandOutputInfo& outputOf(void* data)
    {
        return *static_cast<WaylandOutputInfo*>(data);
    }

    static void global(void* data,
                       wl_registry* registry,
                       uint32_t name,
                       const char* interface,
                       uint32_t version)
    {
        auto& display = self(data);
        auto named = [interface](const wl_interface& candidate)
        { return std::strcmp(interface, candidate.name) == 0; };

        if (named(wl_compositor_interface))
        {
            display.compositor =
                waylandBind<wl_compositor>(registry,
                                           name,
                                           &wl_compositor_interface,
                                           version,
                                           waylandCompositorVersion);
        }
        else if (named(wl_subcompositor_interface))
        {
            display.subcompositor = waylandBind<wl_subcompositor>(
                registry, name, &wl_subcompositor_interface, version, 1);
        }
        else if (named(wl_shm_interface))
        {
            display.shm =
                waylandBind<wl_shm>(registry, name, &wl_shm_interface, version, 1);
        }
        else if (named(wl_seat_interface))
        {
            display.seat = waylandBind<wl_seat>(
                registry, name, &wl_seat_interface, version, waylandSeatVersion);

            if (display.input != nullptr)
                display.input->setSeat(display.seat);
        }
        else if (named(xdg_wm_base_interface))
        {
            display.xdgShell = waylandBind<xdg_wm_base>(registry,
                                                        name,
                                                        &xdg_wm_base_interface,
                                                        version,
                                                        waylandXdgShellVersion);
            xdg_wm_base_add_listener(display.xdgShell, &shellListener, data);
        }
        else if (named(wp_viewporter_interface))
        {
            display.viewporter = waylandBind<wp_viewporter>(
                registry, name, &wp_viewporter_interface, version, 1);
        }
        else if (named(wp_fractional_scale_manager_v1_interface))
        {
            display.fractionalScales = waylandBind<wp_fractional_scale_manager_v1>(
                registry,
                name,
                &wp_fractional_scale_manager_v1_interface,
                version,
                1);
        }
        else if (named(zwp_pointer_constraints_v1_interface))
        {
            display.pointerConstraints = waylandBind<zwp_pointer_constraints_v1>(
                registry, name, &zwp_pointer_constraints_v1_interface, version, 1);
        }
        else if (named(zwp_relative_pointer_manager_v1_interface))
        {
            display.relativePointers = waylandBind<zwp_relative_pointer_manager_v1>(
                registry,
                name,
                &zwp_relative_pointer_manager_v1_interface,
                version,
                1);
        }
        else if (named(wl_data_device_manager_interface))
        {
            display.dataDevices = waylandBind<wl_data_device_manager>(
                registry,
                name,
                &wl_data_device_manager_interface,
                version,
                waylandDataDeviceVersion);
        }
        else if (named(zxdg_output_manager_v1_interface))
        {
            display.xdgOutputManager = waylandBind<zxdg_output_manager_v1>(
                registry,
                name,
                &zxdg_output_manager_v1_interface,
                version,
                waylandXdgOutputVersion);
        }
        else if (named(wl_output_interface))
        {
            display.outputs.add(std::make_unique<WaylandOutputInfo>());

            auto& info = *display.outputs.back();
            info.globalName = name;
            info.output = waylandBind<wl_output>(
                registry, name, &wl_output_interface, version, waylandOutputVersion);

            wl_output_add_listener(info.output, &outputListener, &info);
        }
    }

    // wl_output.release exists only from version 3.
    static void releaseOutput(WaylandOutputInfo& info)
    {
        if (info.xdgOutput != nullptr)
            zxdg_output_v1_destroy(info.xdgOutput);

        if (info.output == nullptr)
            return;

        if (wl_output_get_version(info.output) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
            wl_output_release(info.output);
        else
            wl_output_destroy(info.output);
    }

    static void globalRemove(void* data, wl_registry*, uint32_t name)
    {
        auto& display = self(data);

        display.outputs.removeIndexesMatching(
            [name](const std::unique_ptr<WaylandOutputInfo>& info)
            {
                if (info->globalName != name)
                    return false;

                releaseOutput(*info);
                return true;
            });
    }

    static void ping(void*, xdg_wm_base* shell, uint32_t serial)
    {
        xdg_wm_base_pong(shell, serial);
    }

    static void outputGeometry(void* data,
                               wl_output*,
                               int32_t x,
                               int32_t y,
                               int32_t,
                               int32_t,
                               int32_t,
                               const char*,
                               const char*,
                               int32_t)
    {
        outputOf(data).position = {(float) x, (float) y};
    }

    static void outputMode(void* data,
                           wl_output*,
                           uint32_t flags,
                           int32_t width,
                           int32_t height,
                           int32_t refresh)
    {
        if ((flags & WL_OUTPUT_MODE_CURRENT) == 0)
            return;

        auto& info = outputOf(data);
        info.modeSize = {(float) width, (float) height};
        info.refreshMilliHz = refresh > 0 ? refresh : waylandDefaultRefreshMilliHz;
    }

    static void outputScale(void* data, wl_output*, int32_t factor)
    {
        outputOf(data).scale = factor > 0 ? factor : 1;
    }

    static void outputDone(void* data, wl_output*)
    {
        outputOf(data).configured = true;
    }

    static void outputName(void*, wl_output*, const char*) {}
    static void outputDescription(void*, wl_output*, const char*) {}

    static void xdgOutputPosition(void* data, zxdg_output_v1*, int32_t x, int32_t y)
    {
        outputOf(data).position = {(float) x, (float) y};
    }

    static void xdgOutputSize(void* data, zxdg_output_v1*, int32_t w, int32_t h)
    {
        auto& info = outputOf(data);
        info.xdgLogicalSize = {(float) w, (float) h};
        info.hasXdgLogicalSize = w > 0 && h > 0;
    }

    static void xdgOutputDone(void* data, zxdg_output_v1*)
    {
        outputOf(data).configured = true;
    }

    static void xdgOutputName(void*, zxdg_output_v1*, const char*) {}
    static void xdgOutputDescription(void*, zxdg_output_v1*, const char*) {}

    static const wl_registry_listener registryListener;
    static const wl_output_listener outputListener;
    static const zxdg_output_v1_listener xdgOutputListener;
    static const xdg_wm_base_listener shellListener;
};

const wl_registry_listener WaylandRegistryDispatch::registryListener {
    .global = WaylandRegistryDispatch::global,
    .global_remove = WaylandRegistryDispatch::globalRemove,
};

const wl_output_listener WaylandRegistryDispatch::outputListener {
    .geometry = WaylandRegistryDispatch::outputGeometry,
    .mode = WaylandRegistryDispatch::outputMode,
    .done = WaylandRegistryDispatch::outputDone,
    .scale = WaylandRegistryDispatch::outputScale,
    .name = WaylandRegistryDispatch::outputName,
    .description = WaylandRegistryDispatch::outputDescription,
};

const zxdg_output_v1_listener WaylandRegistryDispatch::xdgOutputListener {
    .logical_position = WaylandRegistryDispatch::xdgOutputPosition,
    .logical_size = WaylandRegistryDispatch::xdgOutputSize,
    .done = WaylandRegistryDispatch::xdgOutputDone,
    .name = WaylandRegistryDispatch::xdgOutputName,
    .description = WaylandRegistryDispatch::xdgOutputDescription,
};

const xdg_wm_base_listener WaylandRegistryDispatch::shellListener {
    .ping = WaylandRegistryDispatch::ping,
};

// A plugin failure leaves libdecor's fallback: the window goes up undecorated.
struct WaylandDecorationDispatch
{
    static void error(libdecor*, enum libdecor_error, const char* message)
    {
        LOG("libdecor: ", message ? message : "unknown error");
    }

    static libdecor_interface interface;
};

// Zero-initialised: the struct carries reserved slots libdecor never uses.
libdecor_interface WaylandDecorationDispatch::interface = []
{
    auto table = libdecor_interface {};
    table.error = WaylandDecorationDispatch::error;

    return table;
}();

WaylandShmBuffer::~WaylandShmBuffer()
{
    destroy();
}

void WaylandShmBuffer::destroy()
{
    if (buffer != nullptr)
    {
        wl_buffer_destroy(buffer);
        buffer = nullptr;
    }

    if (pixels != nullptr)
    {
        ::munmap(pixels, byteSize);
        pixels = nullptr;
    }

    byteSize = 0;
    width = 0;
    height = 0;
}

bool WaylandShmBuffer::create(wl_shm* shm, int w, int h, Color colour)
{
    destroy();

    if (shm == nullptr || w <= 0 || h <= 0)
        return false;

    const auto stride = w * 4;
    byteSize = (size_t) stride * (size_t) h;

    auto fd = ::memfd_create("eacp-wayland", MFD_CLOEXEC | MFD_ALLOW_SEALING);

    if (fd < 0)
    {
        byteSize = 0;
        return false;
    }

    if (::ftruncate(fd, (off_t) byteSize) != 0)
    {
        ::close(fd);
        byteSize = 0;
        return false;
    }

    pixels = ::mmap(nullptr, byteSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (pixels == MAP_FAILED)
    {
        pixels = nullptr;
        ::close(fd);
        byteSize = 0;
        return false;
    }

    auto* pool = wl_shm_create_pool(shm, fd, (int32_t) byteSize);
    buffer =
        wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    ::close(fd);

    auto* words = static_cast<uint32_t*>(pixels);
    std::fill(
        words, words + (size_t) w * (size_t) h, waylandPremultipliedPixel(colour));

    width = w;
    height = h;

    return buffer != nullptr;
}

WaylandDisplay::WaylandDisplay()
{
    display = wl_display_connect(nullptr);

    if (display == nullptr)
    {
        LOG("Wayland: could not connect to the compositor named by "
            "WAYLAND_DISPLAY. Windows will be created without a surface, as "
            "they are under EACP_HEADLESS.");
        return;
    }

    input = std::make_unique<WaylandInput>(*this);

    bindGlobals();
    openLoopSource();
}

WaylandDisplay::~WaylandDisplay()
{
    closeLoopSource();

    clipboard.reset();
    input.reset();

    if (decorations != nullptr)
        libdecor_unref(decorations);

    for (auto& info: outputs)
        WaylandRegistryDispatch::releaseOutput(*info);

    if (display != nullptr)
        wl_display_disconnect(display);
}

void WaylandDisplay::bindGlobals()
{
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(
        registry, &WaylandRegistryDispatch::registryListener, this);

    // The second delivers what the objects bound during the first have sent.
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (xdgOutputManager != nullptr)
    {
        for (auto& info: outputs)
        {
            info->xdgOutput = zxdg_output_manager_v1_get_xdg_output(xdgOutputManager,
                                                                    info->output);
            zxdg_output_v1_add_listener(info->xdgOutput,
                                        &WaylandRegistryDispatch::xdgOutputListener,
                                        info.get());
        }

        wl_display_roundtrip(display);
    }

    // Before libdecor, whose GTK plugin makes a wl_data_device of its own on
    // this connection: a compositor answers one per client, and the one it
    // picks is not ours if GTK got there first.
    clipboard = std::make_unique<WaylandClipboard>(*this);

    // Last: libdecor wants a connection whose first round trips are done.
    decorations = libdecor_new(display, &WaylandDecorationDispatch::interface);

    if (decorations == nullptr)
        LOG("Wayland: libdecor could not be initialised; windows will have no "
            "decorations.");
}

void WaylandDisplay::openLoopSource()
{
    loopFd = wl_display_get_fd(display);

    Threads::addLoopSource(
        loopFd, POLLIN, [this] { readAndDispatch(); }, [this] { prepareForPoll(); });

    // A libdecor plugin may hold a connection of its own; the built-in
    // fallback shares ours, and re-registering that fd replaces the source.
    auto decorationsFd = decorations != nullptr ? libdecor_get_fd(decorations) : -1;

    if (decorationsFd >= 0 && decorationsFd != loopFd)
    {
        decorationsLoopFd = decorationsFd;

        Threads::addLoopSource(decorationsLoopFd,
                               POLLIN,
                               [this] { libdecor_dispatch(decorations, 0); });
    }
}

void WaylandDisplay::closeLoopSource()
{
    if (decorationsLoopFd >= 0)
    {
        Threads::removeLoopSource(decorationsLoopFd);
        decorationsLoopFd = -1;
    }

    if (loopFd >= 0)
    {
        Threads::removeLoopSource(loopFd);
        loopFd = -1;
    }
}

// Only any use immediately before poll(): the dispatch drains what another
// reader queued, the flush sends requests still in libwayland's buffer.
void WaylandDisplay::prepareForPoll()
{
    if (!isConnected())
        return;

    if (wl_display_dispatch_pending(display) < 0)
    {
        connectionLost();
        return;
    }

    flush();
}

// prepare_read fails while the default queue has events: drain and retry.
void WaylandDisplay::readAndDispatch()
{
    if (!isConnected())
        return;

    while (wl_display_prepare_read(display) != 0)
    {
        if (wl_display_dispatch_pending(display) < 0)
        {
            connectionLost();
            return;
        }
    }

    // A compositor that went away reads as end of file here.
    if (wl_display_read_events(display) < 0
        || wl_display_dispatch_pending(display) < 0)
    {
        connectionLost();
        return;
    }

    // Redundant while libdecor's plugin shares this connection.
    if (decorations != nullptr)
        libdecor_dispatch(decorations, 0);

    flush();
}

// Everything the compositor owned is dropped, and the process goes on with the
// windows it has left surfaceless - the state a build with no compositor is in
// from the start. There is no reconnect: object ids and buffers died with the
// connection, so the only honest answer is the headless one.
void WaylandDisplay::connectionLost()
{
    if (!isConnected())
        return;

    LOG("Wayland: the connection to the compositor was lost. Windows are now "
        "surfaceless, as they are under EACP_HEADLESS.");

    // First, so nothing below builds a surface on a dead connection.
    compositor = nullptr;
    subcompositor = nullptr;
    shm = nullptr;
    seat = nullptr;
    xdgShell = nullptr;
    viewporter = nullptr;
    fractionalScales = nullptr;
    pointerConstraints = nullptr;
    relativePointers = nullptr;
    xdgOutputManager = nullptr;
    dataDevices = nullptr;

    closeLoopSource();

    clipboard.reset();
    input.reset();

    // Snapshotted: every one of these drops the surfaces it registered.
    auto windows = Vector<WaylandWindowSurface*> {};

    for (const auto& target: surfaces)
        if (target.window != nullptr && !windows.contains(target.window))
            windows.add(target.window);

    for (auto* window: windows)
        window->onConnectionLost();

    surfaces.clear();

    if (decorations != nullptr)
    {
        libdecor_unref(decorations);
        decorations = nullptr;
    }
}

// EAGAIN is a full buffer, not a dead connection: the loop's next poll sends
// the rest.
void WaylandDisplay::flush()
{
    if (!isConnected())
        return;

    if (wl_display_flush(display) < 0 && errno != EAGAIN)
        connectionLost();
}

void WaylandDisplay::roundtrip()
{
    if (!isConnected())
        return;

    if (wl_display_roundtrip(display) < 0)
        connectionLost();
}

const WaylandOutputInfo* WaylandDisplay::getPrimaryOutput() const
{
    // The first the compositor announced: Wayland names no primary output.
    for (const auto& info: outputs)
        if (info->configured)
            return info.get();

    return outputs.empty() ? nullptr : outputs[0].get();
}

float WaylandDisplay::getFallbackScale() const
{
    if (const auto* primary = getPrimaryOutput())
        return (float) std::max(primary->scale, 1);

    return linuxDefaultBackingScale;
}

void WaylandDisplay::registerSurface(const WaylandSurfaceTarget& target)
{
    auto* surface = target.surface;

    surfaces.removeIndexesMatching([surface](const WaylandSurfaceTarget& existing)
                                   { return existing.surface == surface; });

    surfaces.add(target);
}

void WaylandDisplay::unregisterSurface(wl_surface* surface)
{
    // Focus must be dropped before the entry that would route to a dead view.
    if (input != nullptr)
        input->surfaceDestroyed(surface);

    surfaces.removeIndexesMatching([surface](const WaylandSurfaceTarget& target)
                                   { return target.surface == surface; });
}

WaylandSurfaceTarget WaylandDisplay::findSurface(wl_surface* surface) const
{
    for (const auto& target: surfaces)
        if (target.surface == surface)
            return target;

    return {};
}

// Deliberately leaked, as Singleton::getImmortal is: the destructor would
// deregister from an event loop that may already be gone at static teardown.
WaylandDisplay* waylandDisplay()
{
    static auto* instance = []() -> WaylandDisplay*
    {
        if (Apps::getAppEnvironment().headless)
            return nullptr;

        if (!waylandEnvironmentNamesACompositor())
            return nullptr;

        auto* opened = new WaylandDisplay();

        if (!opened->isValid())
        {
            delete opened;
            return nullptr;
        }

        return opened;
    }();

    return instance;
}
} // namespace eacp::Graphics
