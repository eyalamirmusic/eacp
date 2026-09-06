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

// The process's one connection to the compositor, and the pieces of it every
// other Wayland file in this directory shares.
//
// Internal to eacp-graphics: eacp-wayland is linked PRIVATE, so nothing above
// this library ever sees a Wayland type. What the GPU module gets is the two
// opaque pointers in View/View-Linux.h and nothing else.
//
// Every file-scope name here and in the .cpp files that include it carries a
// wayland/Wayland prefix, because EACP_CI_BUILD compiles eacp-graphics as a
// unity build: Window-Linux.cpp, View-Linux.cpp, Display-Linux.cpp,
// Keyboard-Linux.cpp and the two Wayland files land in one translation unit,
// where an anonymous namespace is no protection at all.

namespace eacp::Graphics
{
class View;
class WaylandInput;

// One wl_output, as much of it as has arrived. The compositor sends geometry,
// mode, scale and (through zxdg_output_v1) a logical size as separate events
// and then a done, so nothing here is trustworthy until `configured`.
struct WaylandOutputInfo
{
    // The logical size in points: xdg_output's if the compositor offers the
    // extension, otherwise the mode divided by the integer scale, which is the
    // same figure for every non-fractional setup.
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

// The window-shaped half of a Wayland surface, as everything that is not
// Window-Linux.cpp needs to see it: the surface an input event names, the view
// tree the event is routed into, and the geometry it is measured in.
//
// Window::Native derives from this rather than being reached through a
// forward-declared pointer, so the input translation and the view-surface code
// can do their work without seeing what else a Window is (an icon, a quit
// callback, a libdecor frame).
struct WaylandWindowSurface
{
    // The toplevel's own wl_surface. Null while the window is headless or the
    // connection failed, which is the state the whole backend degrades to.
    wl_surface* surface = nullptr;

    // The view the window adopted, or null before setContentView.
    View* contentView = nullptr;

    // The content size in points - what onResize reports and what a pointer
    // position on `surface` is measured in.
    Point contentSize;

    // Pixels per point the compositor asked for on this surface.
    float scale = linuxDefaultBackingScale;

    // True between the first configure that mapped the toplevel and the unmap
    // that takes it away. A view surface only exists while this holds.
    bool mapped = false;

    // Window::setMouseLocked's intent, which the input code turns into a real
    // pointer lock while the window also has keyboard focus.
    bool mouseLockIntent = false;

    // Called by the input code when the compositor moves keyboard focus onto
    // or off this window. Window-Linux.cpp turns it into
    // WindowEvents::onActivationChanged and engages or drops the mouse lock.
    std::function<void(bool)> onKeyboardFocus = [](bool) {};
};

// Who a wl_surface belongs to. Pointer events name whichever surface they are
// over - the toplevel, or one of the subsurfaces a presenting view was given -
// so translating one into a MouseEvent starts by looking the surface up here.
struct WaylandSurfaceTarget
{
    wl_surface* surface = nullptr;
    WaylandWindowSurface* window = nullptr;

    // Null for the window's own toplevel surface; the presenting view for a
    // subsurface, whose origin inside the window is summed from the view's
    // parent chain at the moment the event arrives rather than cached here.
    View* view = nullptr;
};

// A wl_shm buffer of one solid colour.
//
// The backend needs exactly one kind of buffer of its own: something to put on
// the toplevel surface, because a Wayland surface with no buffer is not mapped
// and an unmapped toplevel gets no configure, no input and no frame callbacks.
// The window's actual picture comes from the swapchain on a subsurface, so
// this is a background and nothing more - 1x1 stretched by a viewport where
// the compositor has wp_viewporter, full size where it does not.
class WaylandShmBuffer
{
public:
    WaylandShmBuffer() = default;
    ~WaylandShmBuffer();

    WaylandShmBuffer(const WaylandShmBuffer&) = delete;
    WaylandShmBuffer& operator=(const WaylandShmBuffer&) = delete;

    // Allocates and fills. Returns false when the shm pool could not be made,
    // which leaves the window unmapped rather than crashing.
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

// The connection, the globals bound off its registry, and the surface
// bookkeeping the rest of the backend reads.
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

    // The output an app should size its first window against, or null when the
    // compositor advertised none.
    const WaylandOutputInfo* getPrimaryOutput() const;

    // The scale to fall back on when a surface has no fractional-scale object
    // and the compositor is too old to send preferred_buffer_scale: the
    // primary output's integer scale, which is what every pre-fractional
    // client used.
    float getFallbackScale() const;

    void registerSurface(const WaylandSurfaceTarget& target);
    void unregisterSurface(wl_surface* surface);
    WaylandSurfaceTarget findSurface(wl_surface* surface) const;

    // Every request made so far, out to the compositor. Called from the loop's
    // prepare hook, so nothing waits on a reply to something still sitting in
    // libwayland's buffer.
    void flush();

    // A blocking round trip. Only used at startup, to turn the registry's
    // announcements into bound globals before the first window is built.
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

    // Held by pointer because each one is the payload of a wl_output listener:
    // a compositor announcing a second monitor would otherwise reallocate the
    // vector and leave the first listener writing into freed memory.
    Vector<std::unique_ptr<WaylandOutputInfo>> outputs;

    Vector<WaylandSurfaceTarget> surfaces;

    std::unique_ptr<WaylandInput> input;

    int loopFd = -1;
    int decorationsLoopFd = -1;
};

// The process's connection, opened on the first call and kept for the life of
// the process.
//
// Null - and no attempt made - when Apps::getAppEnvironment().headless is set,
// when the environment names no compositor, and when the connection failed.
// Every caller treats that as "no window system", which is exactly the headless
// backend this replaced, so a CI runner with no session behaves as it did
// before any of this existed.
WaylandDisplay* waylandDisplay();
} // namespace eacp::Graphics
