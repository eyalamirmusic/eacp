#include "Window.h"

#include "../View/WaylandViewSurface-Linux.h"
#include "WaylandDisplay-Linux.h"
#include "WaylandInput-Linux.h"

#include <algorithm>
#include <cmath>

// The Linux Window: one wl_surface, one libdecor frame, and the same headless
// mode every other backend has.
//
// The shape is the Windows one, and deliberately so. There is one surface per
// Window and none per View, the view tree is composited by the window rather
// than by the OS, and all input is routed by the portable hit-tester - which is
// what lets a presenting view's wl_subsurface be an addition to View-Linux.cpp
// rather than a second window implementation.
//
// Two things Wayland does not have, and this file does not pretend to:
//
// A window has no position. There are no global coordinates in the protocol at
// all - a client is never told where its surface is, and cannot ask to be put
// anywhere - so getPosition/setPosition keep the value the app last supplied
// and onMoved reports the app's own moves, which is what the headless backend
// did and all the contract in Window.h can honestly mean here.
//
// And a window cannot raise itself. toFront on Wayland is a mapped surface and
// nothing more; raising is the compositor's decision, made from a user gesture
// it holds an activation token for. So toFront shows the window and stops.
//
// Under Apps::getAppEnvironment().headless, and on any machine where the
// connection failed, none of this happens: the window is built, answers its
// geometry honestly, and never becomes visible. That is the mode GraphicsTests
// runs in, and it is the behaviour this file replaced.

namespace eacp::Graphics
{
namespace
{
// What the compositor is told this application is, for the task-switcher entry
// and the icon. Wayland has no per-window icon: the app id names a .desktop
// file and the desktop reads the icon out of that, so
// WindowOptions::applicationIcon has nothing to act on here.
constexpr const char* waylandDefaultAppId = "eacp";

// Nothing paints behind a Linux window's content - there is no 2D context to
// paint with - so the toplevel's buffer is one solid colour, and this is that
// colour when WindowOptions names none. Opaque, because a fully transparent
// buffer under an opaque region is undefined, and black rather than white
// because that is the flash Window.h names as the one worth avoiding.
constexpr Color waylandDefaultWindowBackground = Color::gray(0.f);

void waylandNotifyHostVisibility(View* view, bool visible)
{
    if (view == nullptr)
        return;

    view->hostWindowVisibilityChanged(visible);

    for (auto* child: view->getSubviews())
        waylandNotifyHostVisibility(child, visible);
}

bool waylandFlagSet(const WindowOptions& options, WindowFlags flag)
{
    return options.flags.contains(flag);
}
} // namespace

struct Window::Native : WaylandWindowSurface
{
    Native(const WindowOptions& optionsToUse, WindowEvents& eventsToUse)
        : title(optionsToUse.title)
        , quitCallback(optionsToUse.effectiveOnQuit())
        , onResize(optionsToUse.onResize)
        , onWillResize(optionsToUse.onWillResize)
        , events(&eventsToUse)
        , minWidth(optionsToUse.minWidth)
        , minHeight(optionsToUse.minHeight)
        , aspectRatio(optionsToUse.hasAspectRatio() ? optionsToUse.aspectRatio
                                                    : std::optional<Point> {})
        , hidesOnClose(optionsToUse.hidesOnClose)
        , resizable(waylandFlagSet(optionsToUse, WindowFlags::Resizable))
        , closable(waylandFlagSet(optionsToUse, WindowFlags::Closable))
        , miniaturizable(waylandFlagSet(optionsToUse, WindowFlags::Miniaturizable))
        , transparent(optionsToUse.transparentBackground)
        , background(
              optionsToUse.backgroundColor.value_or(waylandDefaultWindowBackground))
    {
        contentSize = {(float) optionsToUse.width, (float) optionsToUse.height};

        if (optionsToUse.initialPosition)
            position = *optionsToUse.initialPosition;

        if (transparent)
            background = Color {0.f, 0.f, 0.f, 0.f};

        // Keyboard focus is what the input code calls activation, and what
        // WindowEvents::onActivationChanged reports. It arrives from the seat
        // rather than from the frame, hence the hook rather than a call.
        onKeyboardFocus = [this](bool focused) { keyboardFocusChanged(focused); };

        createSurface();
    }

    ~Native()
    {
        // Before anything else: a presenting view's swapchain has to be gone
        // before the wl_surface it was made from, and onLost is what tells the
        // GPU side to destroy it.
        if (contentView != nullptr)
            waylandUnbindWindowFromContentView(*contentView);

        destroyFrame();

        if (auto* connection = waylandDisplay())
        {
            if (auto* seatInput = connection->getInput())
                seatInput->windowDestroyed(*this);

            if (surface != nullptr)
                connection->unregisterSurface(surface);
        }

        buffer.destroy();

        if (fractionalScale != nullptr)
            wp_fractional_scale_v1_destroy(fractionalScale);

        if (viewport != nullptr)
            wp_viewport_destroy(viewport);

        if (surface != nullptr)
            wl_surface_destroy(surface);
    }

    // --- setup ----------------------------------------------------------------

    void createSurface()
    {
        auto* connection = waylandDisplay();

        if (connection == nullptr || connection->getCompositor() == nullptr)
            return;

        surface = wl_compositor_create_surface(connection->getCompositor());

        if (surface == nullptr)
            return;

        wl_surface_add_listener(surface, &surfaceListener(), this);
        connection->registerSurface({surface, this, nullptr});

        if (auto* viewporter = connection->getViewporter())
            viewport = wp_viewporter_get_viewport(viewporter, surface);

        // The compositor's own answer to "how many pixels per point", where it
        // has one. Everything else - preferred_buffer_scale below, the output's
        // integer scale - is a coarser fallback for a compositor without this
        // extension, which today includes a headless Weston.
        if (auto* scales = connection->getFractionalScales())
        {
            fractionalScale =
                wp_fractional_scale_manager_v1_get_fractional_scale(scales, surface);
            wp_fractional_scale_v1_add_listener(
                fractionalScale, &fractionalScaleListener(), this);
        }

        scale = connection->getFallbackScale();
    }

    void createFrame()
    {
        auto* connection = waylandDisplay();

        if (frame != nullptr || surface == nullptr || connection == nullptr)
            return;

        auto* decorations = connection->getDecorations();

        if (decorations == nullptr)
            return;

        frame = libdecor_decorate(decorations, surface, &frameListener(), this);

        if (frame == nullptr)
            return;

        libdecor_frame_set_title(frame, title.c_str());
        libdecor_frame_set_app_id(frame, waylandDefaultAppId);

        if (minWidth > 0 || minHeight > 0)
            libdecor_frame_set_min_content_size(
                frame, std::max(minWidth, 1), std::max(minHeight, 1));

        // A window that cannot be resized says so through its capabilities,
        // which is both what draws the decorations without a maximise button
        // and what refuses an interactive resize. The size is pinned as well,
        // because a compositor may configure a size no client asked for.
        if (!resizable)
        {
            libdecor_frame_unset_capabilities(
                frame,
                (enum libdecor_capabilities)(LIBDECOR_ACTION_RESIZE
                                             | LIBDECOR_ACTION_FULLSCREEN));

            auto width = std::max((int) std::lround(contentSize.x), 1);
            auto height = std::max((int) std::lround(contentSize.y), 1);

            libdecor_frame_set_min_content_size(frame, width, height);
            libdecor_frame_set_max_content_size(frame, width, height);
        }

        if (!closable)
            libdecor_frame_unset_capabilities(frame, LIBDECOR_ACTION_CLOSE);

        if (!miniaturizable)
            libdecor_frame_unset_capabilities(frame, LIBDECOR_ACTION_MINIMIZE);

        libdecor_frame_map(frame);
        connection->flush();
    }

    void destroyFrame()
    {
        if (frame == nullptr)
            return;

        libdecor_frame_unref(frame);
        frame = nullptr;
    }

    // --- configure -------------------------------------------------------------

    // The compositor has proposed a size. Everything WindowOptions has to say
    // about the shapes this window may take is applied here, because a
    // configure is the only moment Wayland offers to say it: there is no
    // WM_SIZING to clamp and no NSWindow attribute to set.
    void configure(libdecor_configuration* configuration)
    {
        auto width = std::max((int) std::lround(contentSize.x), 1);
        auto height = std::max((int) std::lround(contentSize.y), 1);

        auto proposedWidth = 0;
        auto proposedHeight = 0;

        if (libdecor_configuration_get_content_size(
                configuration, frame, &proposedWidth, &proposedHeight))
        {
            width = proposedWidth;
            height = proposedHeight;
        }

        applyConstraints(width, height);

        auto* state = libdecor_state_new(width, height);
        libdecor_frame_commit(frame, state, configuration);
        libdecor_state_free(state);

        auto windowState = LIBDECOR_WINDOW_STATE_NONE;

        if (libdecor_configuration_get_window_state(configuration, &windowState))
        {
            maximized = (windowState & LIBDECOR_WINDOW_STATE_MAXIMIZED) != 0;
            setActive((windowState & LIBDECOR_WINDOW_STATE_ACTIVE) != 0);
        }

        resizeTo({(float) width, (float) height});
        present();

        if (!mapped)
        {
            mapped = true;
            waylandNotifyHostVisibility(contentView, true);
        }

        if (contentView != nullptr)
            waylandWindowSurfaceStateChanged(*contentView);
    }

    void applyConstraints(int& width, int& height) const
    {
        if (onWillResize)
            onWillResize(width, height);

        if (aspectRatio)
        {
            // Width drives, height follows. Which side gives way is decided by
            // the resize edge on Windows and by AppKit on macOS; libdecor's
            // configuration carries no edge, so there is one rule, and it is
            // the one a horizontal drag reads best against.
            const auto ratio = aspectRatio->x / aspectRatio->y;
            height = (int) std::lround((float) width / ratio);
        }

        width = std::max(width, std::max(minWidth, 1));
        height = std::max(height, std::max(minHeight, 1));
    }

    void resizeTo(Point newSize)
    {
        if (newSize.x == contentSize.x && newSize.y == contentSize.y)
            return;

        contentSize = newSize;

        if (contentView != nullptr)
            contentView->setBounds({0.f, 0.f, contentSize.x, contentSize.y});

        if (onResize)
            onResize((int) contentSize.x, (int) contentSize.y);
    }

    // Puts the window's background on screen and, with it, whatever
    // wl_subsurface positions were queued since the last commit: a subsurface's
    // placement is applied by its PARENT's commit, so this is also how a moved
    // view lands.
    void present()
    {
        auto* connection = waylandDisplay();

        if (surface == nullptr || connection == nullptr)
            return;

        auto width = std::max((int) std::lround(contentSize.x), 1);
        auto height = std::max((int) std::lround(contentSize.y), 1);

        if (viewport != nullptr)
        {
            // One pixel, stretched. A solid colour needs no more than that, and
            // it makes a resize cost one viewport request rather than a new
            // shared-memory buffer per frame of the drag.
            if (buffer.get() == nullptr)
                buffer.create(connection->getShm(), 1, 1, background);

            wp_viewport_set_destination(viewport, width, height);
        }
        else if (buffer.getWidth() != width || buffer.getHeight() != height)
        {
            buffer.create(connection->getShm(), width, height, background);
        }

        if (buffer.get() == nullptr)
            return;

        applyOpaqueRegion(width, height);

        wl_surface_attach(surface, buffer.get(), 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_commit(surface);

        connection->flush();
    }

    // Telling the compositor which part of the surface it need not blend saves
    // it the whole window's worth of alpha work, and is skipped exactly when
    // the window has asked to be see-through.
    void applyOpaqueRegion(int width, int height)
    {
        auto* connection = waylandDisplay();

        if (connection == nullptr || connection->getCompositor() == nullptr)
            return;

        if (transparent)
        {
            wl_surface_set_opaque_region(surface, nullptr);
            return;
        }

        auto* region = wl_compositor_create_region(connection->getCompositor());
        wl_region_add(region, 0, 0, width, height);
        wl_surface_set_opaque_region(surface, region);
        wl_region_destroy(region);
    }

    // --- state ------------------------------------------------------------------

    void setContentView(View* view)
    {
        contentView = view;

        if (contentView == nullptr)
            return;

        contentView->setBounds({0.f, 0.f, contentSize.x, contentSize.y});

        waylandBindWindowToContentView(*contentView, *this);

        // Shown on adoption, exactly as on Windows and for the same reason:
        // portable app code constructs a window, gives it a view and expects to
        // see it. There is nothing to show when no compositor was reached.
        if (surface != nullptr)
            setVisible(true);
    }

    void setVisible(bool shouldBeVisible)
    {
        if (surface == nullptr)
            return;

        if (shouldBeVisible)
        {
            createFrame();
            return;
        }

        if (mapped || frame != nullptr)
            unmap();
    }

    // Hiding is an unmap: a Wayland surface with no buffer attached is not on
    // screen, and its xdg_toplevel goes with it. The frame is torn down with it
    // and rebuilt on the way back, because an xdg_surface's initial configure
    // sequence happens once and a remap needs a fresh one.
    void unmap()
    {
        auto wasMapped = mapped;
        mapped = false;

        // Before the surface goes: every presenting view under this window
        // loses its subsurface, and hears about it first.
        if (contentView != nullptr)
            waylandWindowSurfaceStateChanged(*contentView);

        destroyFrame();
        setActive(false);

        wl_surface_attach(surface, nullptr, 0, 0);
        wl_surface_commit(surface);
        buffer.destroy();

        if (auto* connection = waylandDisplay())
            connection->flush();

        if (wasMapped)
            waylandNotifyHostVisibility(contentView, false);
    }

    void setTitle(const std::string& newTitle)
    {
        title = newTitle;

        if (frame != nullptr)
            libdecor_frame_set_title(frame, title.c_str());
    }

    // A window has a frame whether or not it is on screen (Window.h), and on
    // Wayland that frame has no place: what setPosition and initialPosition put
    // in is what getPosition hands back, and a move is reported as a move
    // however it was made. Nothing asks the compositor, because nothing can.
    void setPosition(Point newPosition)
    {
        position = newPosition;
        events->onMoved(position);
    }

    void setActive(bool nowActive)
    {
        if (active == nowActive)
            return;

        active = nowActive;
        events->onActivationChanged(active);
    }

    void keyboardFocusChanged(bool focused) { setActive(focused); }

    void closeRequested()
    {
        // See WindowOptions::hidesOnClose: hide instead of destroy, and say so,
        // because onQuit is exactly what hidesOnClose suppresses.
        if (hidesOnClose)
        {
            unmap();
            events->onHidden();
            return;
        }

        quitCallback();
    }

    void scaleChanged(float newScale)
    {
        if (newScale <= 0.f || newScale == scale)
            return;

        scale = newScale;

        if (contentView == nullptr)
            return;

        // The news has to travel on its own here. A fractional-scale change
        // carries no size with it, so nothing else would tell a glyph atlas
        // rasterized at the old scale that it is now wrong - which is the one
        // Windows behaviour §6 of the plan says not to copy.
        notifyBackingScaleChanged(*contentView);
        waylandWindowSurfaceStateChanged(*contentView);
    }

    void setMouseLocked(bool locked)
    {
        mouseLockIntent = locked;

        if (auto* connection = waylandDisplay())
            if (auto* seatInput = connection->getInput())
                seatInput->updateMouseLock(*this);
    }

    bool hasKeyboardFocus() const
    {
        auto* connection = waylandDisplay();

        if (connection == nullptr || connection->getInput() == nullptr)
            return false;

        return connection->getInput()->getKeyboardFocus() == this;
    }

    // --- listeners ---------------------------------------------------------------

    static Native& self(void* data) { return *static_cast<Native*>(data); }

    // Non-const because libdecor_decorate keeps the pointer it is handed and
    // its signature says nothing about not writing through it.
    static libdecor_frame_interface& frameListener()
    {
        // Built by a lambda rather than by a designated initializer: the struct
        // carries ten reserved slots libdecor has never used, and zeroing them
        // says so without naming any of them.
        static auto table = []
        {
            auto built = libdecor_frame_interface {};

            built.configure = [](libdecor_frame*,
                                 libdecor_configuration* configuration,
                                 void* data)
            { self(data).configure(configuration); };

            built.close = [](libdecor_frame*, void* data)
            { self(data).closeRequested(); };

            // The decorations are drawn on synchronous subsurfaces of ours, so
            // they only reach the screen when the parent commits. This is
            // libdecor asking for that commit.
            built.commit = [](libdecor_frame*, void* data) { self(data).present(); };

            built.dismiss_popup = [](libdecor_frame*, const char*, void*) {};

            return built;
        }();

        return table;
    }

    static const wp_fractional_scale_v1_listener& fractionalScaleListener()
    {
        static const wp_fractional_scale_v1_listener table {
            .preferred_scale =
                [](void* data, wp_fractional_scale_v1*, uint32_t scale120)
            {
                // The protocol's unit is 120ths, so 1.5x arrives as 180.
                self(data).scaleChanged((float) scale120 / 120.f);
            },
        };

        return table;
    }

    static const wl_surface_listener& surfaceListener()
    {
        static const wl_surface_listener table {
            .enter =
                [](void* data, wl_surface*, wl_output*)
            {
                // Which output the surface is on matters only for the scale,
                // and only while there is no fractional-scale object to ask.
                auto& window = self(data);

                if (window.fractionalScale == nullptr)
                    if (auto* connection = waylandDisplay())
                        window.scaleChanged(connection->getFallbackScale());
            },
            .leave = [](void*, wl_surface*, wl_output*) {},
            .preferred_buffer_scale =
                [](void* data, wl_surface*, int32_t factor)
            {
                // The pre-fractional answer, and the one a compositor at
                // wl_compositor version 6 gives: whole pixels per point.
                if (self(data).fractionalScale == nullptr)
                    self(data).scaleChanged((float) std::max(factor, 1));
            },
            .preferred_buffer_transform = [](void*, wl_surface*, uint32_t) {},
        };

        return table;
    }

    std::string title;
    Callback quitCallback;
    ResizeCallback onResize;
    WillResizeCallback onWillResize;
    WindowEvents* events;

    int minWidth = 0;
    int minHeight = 0;
    std::optional<Point> aspectRatio;
    bool hidesOnClose = false;
    bool resizable = true;
    bool closable = true;
    bool miniaturizable = true;
    bool transparent = false;
    Color background;

    Point position;
    bool active = false;
    bool maximized = false;

    libdecor_frame* frame = nullptr;
    wp_viewport* viewport = nullptr;
    wp_fractional_scale_v1* fractionalScale = nullptr;
    WaylandShmBuffer buffer;
};

Window::Window(const WindowOptions& optionsToUse)
    : options(optionsToUse)
    , impl(optionsToUse, events)
{
}

Window::~Window() = default;

void Window::setTitle(const std::string& title)
{
    impl->setTitle(title);
}

// The wl_surface, which is what a VkSurfaceKHR is made from and what a foreign
// host would parent into. Null under headless and wherever no compositor was
// reached, as the whole backend is.
void* Window::getHandle()
{
    return impl->surface;
}

// The content view's own native identity rather than a second surface. There is
// no view-level surface on Linux except the one a presenting view asks for
// through requestViewSurface, and handing the toplevel back twice would tell a
// caller these were different things.
void* Window::getContentViewHandle()
{
    return impl->contentView != nullptr ? impl->contentView->getHandle() : nullptr;
}

void Window::setContentView(View& view)
{
    contentLink.attach(&view, this);
    impl->setContentView(&view);
}

// Nothing to order in front of. A Wayland client cannot raise itself - that is
// the compositor's decision, made from a gesture it holds an activation token
// for - so this shows the window and stops there.
void Window::toFront()
{
    impl->setVisible(true);
}

void Window::setVisible(bool visible)
{
    impl->setVisible(visible);
}

void Window::minimize()
{
    if (impl->frame != nullptr)
        libdecor_frame_set_minimized(impl->frame);
}

void Window::toggleMaximize()
{
    if (impl->frame == nullptr)
        return;

    if (impl->maximized)
        libdecor_frame_unset_maximized(impl->frame);
    else
        libdecor_frame_set_maximized(impl->frame);
}

bool Window::isVisible()
{
    return impl->mapped;
}

Point Window::getPosition() const
{
    return impl->position;
}

void Window::setPosition(Point position)
{
    impl->setPosition(position);
}

void Window::setMouseLocked(bool locked)
{
    impl->setMouseLocked(locked);
}

bool Window::isMouseLocked() const
{
    return impl->mouseLockIntent;
}

// The native unit, which on Linux is the evdev keycode - see Keyboard-Linux.h.
// Answered only while the compositor has given this window keyboard focus, so a
// background window reports nothing pressed however the keyboard is being used
// elsewhere.
bool Window::isKeyPressed(uint16_t nativeKeyCode) const
{
    auto* connection = waylandDisplay();

    if (connection == nullptr || connection->getInput() == nullptr)
        return false;

    if (!impl->hasKeyboardFocus())
        return false;

    return connection->getInput()->isKeyPressed(nativeKeyCode);
}

bool Window::isShiftPressed() const
{
    return getModifiers().shift;
}

bool Window::isControlPressed() const
{
    return getModifiers().control;
}

bool Window::isAltPressed() const
{
    return getModifiers().alt;
}

bool Window::isCommandPressed() const
{
    return getModifiers().command;
}

ModifierKeys Window::getModifiers() const
{
    auto* connection = waylandDisplay();

    if (connection == nullptr || connection->getInput() == nullptr)
        return {};

    if (!impl->hasKeyboardFocus())
        return {};

    return connection->getInput()->getModifiers();
}

} // namespace eacp::Graphics
