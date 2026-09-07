#pragma once

#include "../Primitives/Primitives.h"
#include "../View/View-Linux.h"

#include <wayland-client.h>

#include "fractional-scale-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <libdecor.h>

#include <memory>

// The process's one connection to the compositor. File-scope names carry a
// wayland/Wayland prefix: this is one unity TU under EACP_CI_BUILD.

namespace eacp::Graphics
{
class View;
class WaylandInput;

// Nothing here is trustworthy until `configured`.
struct WaylandOutputInfo
{
    Point logicalSize() const;

    wl_output* output = nullptr;
    zxdg_output_v1* xdgOutput = nullptr;
    uint32_t globalName = 0;

    Point position;
    Point modeSize;
    Point xdgLogicalSize;
    int scale = 1;

    // Millihertz, as wl_output.mode reports it: 60 Hz is 60000.
    int refreshMilliHz = 0;

    bool hasXdgLogicalSize = false;
    bool configured = false;
};

// What the input and view-surface code need of a Window; Window::Native
// derives from it.
struct WaylandWindowSurface
{
    // Null while the window is headless or the connection failed.
    wl_surface* surface = nullptr;

    View* contentView = nullptr;

    // In points, and so are pointer positions on `surface`.
    Point contentSize;

    float scale = linuxDefaultBackingScale;
    bool mapped = false;

    // Intent only; a real pointer lock also needs keyboard focus.
    bool mouseLockIntent = false;

    std::function<void(bool)> onKeyboardFocus = [](bool) {};
};

struct WaylandSurfaceTarget
{
    wl_surface* surface = nullptr;
    WaylandWindowSurface* window = nullptr;

    // Null for the window's own toplevel surface, set for a view's subsurface.
    View* view = nullptr;
};

// A solid colour behind the toplevel: a surface with no buffer is never mapped.
class WaylandShmBuffer
{
public:
    WaylandShmBuffer() = default;
    ~WaylandShmBuffer();

    WaylandShmBuffer(const WaylandShmBuffer&) = delete;
    WaylandShmBuffer& operator=(const WaylandShmBuffer&) = delete;

    // False when the shm pool could not be made.
    bool create(wl_shm* shm, int width, int height, Color colour);

    void destroy();

    wl_buffer* get() const { return buffer; }
    int getWidth() const { return width; }
    int getHeight() const { return height; }

private:
    wl_buffer* buffer = nullptr;
    void* pixels = nullptr;
    size_t byteSize = 0;
    int width = 0;
    int height = 0;
};

class WaylandDisplay
{
public:
    WaylandDisplay();
    ~WaylandDisplay();

    WaylandDisplay(const WaylandDisplay&) = delete;
    WaylandDisplay& operator=(const WaylandDisplay&) = delete;

    bool isValid() const { return display != nullptr; }

    wl_display* getDisplay() const { return display; }
    wl_compositor* getCompositor() const { return compositor; }
    wl_subcompositor* getSubcompositor() const { return subcompositor; }
    wl_shm* getShm() const { return shm; }
    wp_viewporter* getViewporter() const { return viewporter; }
    wl_seat* getSeat() const { return seat; }
    libdecor* getDecorations() const { return decorations; }

    wp_fractional_scale_manager_v1* getFractionalScales() const
    {
        return fractionalScales;
    }

    zwp_pointer_constraints_v1* getPointerConstraints() const
    {
        return pointerConstraints;
    }

    zwp_relative_pointer_manager_v1* getRelativePointers() const
    {
        return relativePointers;
    }

    WaylandInput* getInput() const { return input.get(); }

    // Null when the compositor advertised no output.
    const WaylandOutputInfo* getPrimaryOutput() const;

    float getFallbackScale() const;

    void registerSurface(const WaylandSurfaceTarget& target);
    void unregisterSurface(wl_surface* surface);
    WaylandSurfaceTarget findSurface(wl_surface* surface) const;

    void flush();
    void roundtrip();

private:
    void bindGlobals();
    void openLoopSource();
    void closeLoopSource();
    void prepareForPoll();
    void readAndDispatch();

    friend struct WaylandRegistryDispatch;

    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_subcompositor* subcompositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    xdg_wm_base* xdgShell = nullptr;
    wp_viewporter* viewporter = nullptr;
    wp_fractional_scale_manager_v1* fractionalScales = nullptr;
    zwp_pointer_constraints_v1* pointerConstraints = nullptr;
    zwp_relative_pointer_manager_v1* relativePointers = nullptr;
    zxdg_output_manager_v1* xdgOutputManager = nullptr;
    libdecor* decorations = nullptr;

    // By pointer: each one is a live wl_output listener's payload.
    Vector<std::unique_ptr<WaylandOutputInfo>> outputs;

    Vector<WaylandSurfaceTarget> surfaces;

    std::unique_ptr<WaylandInput> input;

    int loopFd = -1;
    int decorationsLoopFd = -1;
};

// Null when headless or no compositor could be reached.
WaylandDisplay* waylandDisplay();
} // namespace eacp::Graphics
