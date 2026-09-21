#include "GPUView.h"

#include "../Device/Device.h"
#include "../Frame/Frame.h"
#include "../OpenGL/GLBackend-Linux.h"
#include "../OpenGL/GLContext-Linux.h"

#include <eacp/Core/Utils/Logging.h>
#include <eacp/Graphics/View/View-Linux.h>

#include <dlfcn.h>

#include <algorithm>
#include <cstdint>

// The presenting half of a GPUView on the GL backend: an EGLSurface over the
// view's own window-system surface - a wl_egl_window on its subsurface, or its
// X11 child window through EGL_EXT_platform_xcb - drawn into as the default
// framebuffer and handed over with eglSwapBuffers. The interval is zero, so the
// main thread never blocks inside the driver waiting for a vertical blank, and
// the pacing is the frame callback the window system already hands the Vulkan
// backend (plan.md D5, stage 3).
namespace eacp::GPU
{
namespace
{
// The whole of the link to libwayland-egl: three functions by name out of a
// library opened by name, so nothing links it and a machine with no Wayland at
// all builds the same binary (D5).
struct GLWaylandEgl
{
    GLWaylandEgl()
    {
        handle = dlopen("libwayland-egl.so.1", RTLD_LAZY | RTLD_LOCAL);

        if (handle == nullptr)
            handle = dlopen("libwayland-egl.so", RTLD_LAZY | RTLD_LOCAL);

        if (handle == nullptr)
        {
            LOG("OpenGL: no libwayland-egl to open (", dlerror(), ")");
            return;
        }

        create = reinterpret_cast<Create>(dlsym(handle, "wl_egl_window_create"));
        destroy = reinterpret_cast<Destroy>(dlsym(handle, "wl_egl_window_destroy"));
        resize = reinterpret_cast<Resize>(dlsym(handle, "wl_egl_window_resize"));
    }

    bool isValid() const
    {
        return create != nullptr && destroy != nullptr && resize != nullptr;
    }

    // Declared here rather than included: wl_egl_window is opaque and a
    // wl_surface is whatever the record handed over.
    using Create = void* (*) (void* surface, int width, int height);
    using Destroy = void (*)(void* window);
    using Resize = void (*)(void* window, int width, int height, int dx, int dy);

    void* handle = nullptr;
    Create create = nullptr;
    Destroy destroy = nullptr;
    Resize resize = nullptr;
};

const GLWaylandEgl& glWaylandEgl()
{
    static const GLWaylandEgl library;

    return library;
}

// What the surface itself carries, which is what the view has no companion
// framebuffer for.
struct GLSurfacePlanes
{
    int samples = 1;
    bool depth = false;
    bool stencil = false;
};

GLSurfacePlanes glPlanesOf(EGLDisplay display, EGLConfig config)
{
    auto planes = GLSurfacePlanes {};

    planes.samples = std::max(glConfigAttribute(display, config, EGL_SAMPLES), 1);
    planes.depth = glConfigAttribute(display, config, EGL_DEPTH_SIZE) > 0;
    planes.stencil = glConfigAttribute(display, config, EGL_STENCIL_SIZE) > 0;

    return planes;
}

// The colour a multisampled view draws into and the depth plane one the
// surface could not carry draws into, resolved into the default framebuffer at
// the end of every pass. Renderbuffers rather than textures: nothing samples
// them, and a renderbuffer is the only multisampled attachment the floor has.
struct GLViewCompanion
{
    bool isValid() const { return framebuffer != 0; }

    void release()
    {
        if (color != 0)
            glDeleteRenderbuffers(1, &color);

        if (depth != 0)
            glDeleteRenderbuffers(1, &depth);

        if (framebuffer != 0)
            glDeleteFramebuffers(1, &framebuffer);

        *this = {};
    }

    bool create(int width, int height, int samples, bool wantDepth, bool stencil)
    {
        release();

        if (width <= 0 || height <= 0)
            return false;

        glGenFramebuffers(1, &framebuffer);

        if (framebuffer == 0)
            return false;

        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

        glGenRenderbuffers(1, &color);
        glBindRenderbuffer(GL_RENDERBUFFER, color);
        allocate(GL_RGBA8, width, height, samples);
        glFramebufferRenderbuffer(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);

        if (wantDepth)
        {
            glGenRenderbuffers(1, &depth);
            glBindRenderbuffer(GL_RENDERBUFFER, depth);
            allocate(stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24,
                     width,
                     height,
                     samples);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                      stencil ? GL_DEPTH_STENCIL_ATTACHMENT
                                              : GL_DEPTH_ATTACHMENT,
                                      GL_RENDERBUFFER,
                                      depth);
        }

        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        const auto complete =
            glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (complete)
            return true;

        LOG("OpenGL: the view's companion framebuffer came back incomplete, so "
            "it presents without the planes it asked for");

        release();
        return false;
    }

    static void allocate(GLenum format, int width, int height, int samples)
    {
        if (samples > 1)
        {
            glRenderbufferStorageMultisample(
                GL_RENDERBUFFER, (GLsizei) samples, format, width, height);
            return;
        }

        glRenderbufferStorage(GL_RENDERBUFFER, format, width, height);
    }

    GLuint framebuffer = 0;
    GLuint color = 0;
    GLuint depth = 0;
};

struct GLGPUViewBackend final : GPUViewBackend
{
    GLGPUViewBackend(GPUView& viewToUse, Graphics::ViewSurface& recordToUse)
        : view(viewToUse)
        , record(recordToUse)
        , context(getGLContext(Device::shared()))
    {
    }

    ~GLGPUViewBackend() override { destroySurface(); }

    bool surfaceAvailable() override
    {
        if (!createSurface())
            return false;

        companionStale = true;
        return true;
    }

    // Fires before the native surface is torn down: an EGLSurface that outlives
    // the wl_surface or the window it was made from is a use-after-free inside
    // the driver, so it goes here and now rather than when the view does.
    void surfaceLost() override { destroySurface(); }

    void surfaceResized() override { sizeStale = true; }

    bool isPresenting() const override { return surface != EGL_NO_SURFACE; }

    void setSampleCount(int count) override
    {
        sampleCount = count;
        companionStale = true;
    }

    void setDepth(bool depth, bool stencil) override
    {
        if (depthEnabled == depth && stencilEnabled == stencil)
            return;

        depthEnabled = depth;
        stencilEnabled = stencil;

        // The planes may be the surface's own, so this is the one change that
        // wants a different config under the same window - and the one that
        // costs a surface, which is why nothing is done when nothing moved.
        surfaceStale = true;
    }

    // Nothing to spread over slots: a GL context is one stream, and what paces
    // a frame here is the window system's callback rather than an image coming
    // back.
    void setFramesInFlight(int) override {}

    bool createSurface()
    {
        if (surface != EGL_NO_SURFACE)
            return true;

        if (!context.isValid() || !record.handle.isValid())
            return false;

        surfaceStale = false;

        if (!createNativeWindow())
            return false;

        if (createSurfaceOn(context.windowConfigs(depthEnabled, stencilEnabled))
            || createSurfaceOn(context.windowConfigs(false, false)))
            return true;

        reportNoSurface();
        destroyNativeWindow();

        return false;
    }

    bool createSurfaceOn(const Vector<EGLConfig>& configs)
    {
        const auto display = context.getDisplay();

        for (auto candidate: configs)
        {
            auto* made = createPlatformSurface(display, candidate);

            if (made == EGL_NO_SURFACE)
                continue;

            surface = made;
            planes = glPlanesOf(display, candidate);

            // Never blocking in the driver is the whole point: what paces the
            // view is requestFrameCallback, the same as on the Vulkan backend.
            if (context.makeCurrentOn(surface))
            {
                eglSwapInterval(display, 0);
                takeDefaultFramebuffer();
            }

            return true;
        }

        return false;
    }

    EGLSurface createPlatformSurface(EGLDisplay display, EGLConfig candidate)
    {
        // The X11 half names a window id and the Wayland one the wl_egl_window
        // built above; both are what the platform extension calls a native
        // window.
        auto* native = nativeWindow;

        if (record.handle.kind == Graphics::NativeSurfaceHandle::Kind::X11)
            native = &x11Window;

        if (native == nullptr)
            return EGL_NO_SURFACE;

        if (eglCreatePlatformWindowSurface != nullptr)
            return eglCreatePlatformWindowSurface(
                display, candidate, native, nullptr);

        if (eglCreatePlatformWindowSurfaceEXT != nullptr)
            return eglCreatePlatformWindowSurfaceEXT(
                display, candidate, native, nullptr);

        return EGL_NO_SURFACE;
    }

    // A context whose first makeCurrent named no surface at all - which is
    // every context here, the Device coming up long before any view is shown -
    // has GL_NONE for the default framebuffer's draw and read buffer, EGL
    // setting those at the first bind and never again. Everything a view
    // presents lands in that framebuffer, so without this the frames are drawn,
    // swapped and reported fine and the window stays empty.
    void takeDefaultFramebuffer() const
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (glad_glDrawBuffer != nullptr)
        {
            glDrawBuffer(GL_BACK);
        }
        else
        {
            const GLenum back = GL_BACK;
            glDrawBuffers(1, &back);
        }

        glReadBuffer(GL_BACK);
    }

    bool createNativeWindow()
    {
        using Kind = Graphics::NativeSurfaceHandle::Kind;

        switch (record.handle.kind)
        {
            case Kind::Wayland:
                return createWaylandWindow();

            case Kind::X11:
                x11Window = record.handle.window;
                return x11Window != 0;

            case Kind::None:
                break;
        }

        return false;
    }

    bool createWaylandWindow()
    {
        const auto& wayland = glWaylandEgl();

        if (!wayland.isValid() || record.handle.surface == nullptr)
            return false;

        nativeWindow =
            wayland.create(record.handle.surface, pixelWidth(), pixelHeight());

        return nativeWindow != nullptr;
    }

    void destroyNativeWindow()
    {
        if (nativeWindow != nullptr)
            glWaylandEgl().destroy(nativeWindow);

        nativeWindow = nullptr;
        x11Window = 0;
    }

    void destroySurface()
    {
        if (context.isValid())
        {
            context.makeCurrent();
            companion.release();
        }

        if (surface != EGL_NO_SURFACE)
        {
            context.surfaceIsGoing(surface);
            eglDestroySurface(context.getDisplay(), surface);
            surface = EGL_NO_SURFACE;
        }

        // After the EGLSurface, which was made from it.
        destroyNativeWindow();

        companionStale = true;
        surfaceStale = false;
        sizeStale = false;
    }

    static void reportNoSurface()
    {
        static auto reported = false;

        if (reported)
            return;

        reported = true;
        LOG("OpenGL: no EGL config would carry this window, so the view renders "
            "off-screen only");
    }

    int pixelWidth() const { return std::max(record.pixelWidth, 1); }
    int pixelHeight() const { return std::max(record.pixelHeight, 1); }

    // Wayland's buffer size is the client's to name and the compositor takes
    // what it is given; X11's is the window's, and the server has already been
    // told by the view surface that configured it.
    void applyResize()
    {
        sizeStale = false;

        if (nativeWindow != nullptr)
            glWaylandEgl().resize(nativeWindow, pixelWidth(), pixelHeight(), 0, 0);

        companionStale = true;
    }

    int effectiveSampleCount() const
    {
        if (sampleCount <= 1 || !Device::shared().supportsSampleCount(sampleCount))
            return 1;

        return sampleCount;
    }

    // A companion is what the surface itself could not carry: more than one
    // sample, or a depth plane the config had none of. A view that asked for
    // neither draws straight into the default framebuffer and costs no copy.
    void rebuildCompanion()
    {
        companionStale = false;
        companion.release();

        const auto samples = effectiveSampleCount();
        const auto needsDepth = depthEnabled && !planes.depth;
        const auto needsStencil = stencilEnabled && !planes.stencil;

        if (samples <= 1 && !needsDepth && !needsStencil)
            return;

        companion.create(pixelWidth(),
                         pixelHeight(),
                         samples,
                         depthEnabled || stencilEnabled,
                         stencilEnabled);
    }

    bool readyToRender() override
    {
        if (!record.handle.isValid() || !Device::shared().isValid())
            return false;

        if (surfaceStale)
            destroySurface();

        if (surface == EGL_NO_SURFACE && !createSurface())
            return false;

        if (!context.makeCurrentOn(surface))
            return false;

        if (sizeStale)
            applyResize();

        if (companionStale)
            rebuildCompanion();

        return true;
    }

    void renderOneFrame(float scale) override
    {
        auto& gpu = Device::shared();

        auto drawable = GLDrawable {};

        drawable.width = pixelWidth();
        drawable.height = pixelHeight();
        drawable.samples =
            companion.isValid() ? effectiveSampleCount() : planes.samples;
        drawable.depth = depthEnabled;
        drawable.stencil = stencilEnabled;
        drawable.framebuffer = companion.framebuffer;
        drawable.resolveToDefault = companion.isValid();
        drawable.present = [this] { present(); };

        // The first one rides on this present; after that it commits itself.
        record.requestFrameCallback();

        auto frame = Frame(gpu, &drawable, nullptr, nullptr, scale);
        view.render(frame);
    }

    void present()
    {
        if (surface == EGL_NO_SURFACE)
            return;

        if (eglSwapBuffers(context.getDisplay(), surface) == EGL_TRUE)
            return;

        // A surface the window system took away under us: the next available
        // rebuilds it, and until then there is nothing to present to.
        if (eglGetError() == EGL_BAD_NATIVE_WINDOW)
            surfaceStale = true;
    }

    GPUView& view;
    Graphics::ViewSurface& record;
    GLContext& context;

    int sampleCount = 1;
    bool depthEnabled = false;
    bool stencilEnabled = false;

    EGLSurface surface = EGL_NO_SURFACE;
    GLSurfacePlanes planes;

    // The wl_egl_window a Wayland surface is made from, and the X11 window id
    // the xcb platform takes the address of.
    void* nativeWindow = nullptr;
    std::uint32_t x11Window = 0;

    GLViewCompanion companion;

    bool surfaceStale = false;
    bool sizeStale = false;
    bool companionStale = true;
};
} // namespace

std::unique_ptr<GPUViewBackend> makeGLGPUView(GPUView& view,
                                              Graphics::ViewSurface& record)
{
    return std::make_unique<GLGPUViewBackend>(view, record);
}
} // namespace eacp::GPU
