#pragma once

#include "GLCapabilities.h"
#include "GLTypes.h"

#include <glad/egl.h>

// One Device, one context, one thread (plan.md D2).
//
// The EGLDisplay is process-wide and refcounted, like VulkanShared; every
// Device gets an EGLContext of its own on it, made current on the thread that
// built the Device and, headless, over no surface at all. Two Devices on one
// thread are two contexts, and a thread-local "who is current" pointer makes
// eglMakeCurrent happen only when the device changes - a Device used off its
// own thread is already a debug assertion above this layer.
//
// Nothing here links libEGL: the library is opened by name and every entry
// point comes out of it, so a machine with no EGL builds the same binary and
// reports Device::isValid() false.
namespace eacp::GPU
{
class GLContext
{
public:
    GLContext();
    ~GLContext();

    GLContext(const GLContext&) = delete;
    GLContext& operator=(const GLContext&) = delete;

    bool isValid() const { return context != EGL_NO_CONTEXT; }

    // Makes this context current on the calling thread, unless the thread
    // already has it. Called at the top of every operation that touches GL,
    // which is what lets two Devices share a thread. Whichever surface the
    // context has is left alone: everything off-screen draws into a
    // framebuffer object, which no surface is involved in.
    void makeCurrent() const;

    // Makes it current with `surface` as both the draw and the read surface,
    // which is what a view that presents needs and the only thing that puts
    // anything in the default framebuffer. Views on one Device are one context
    // and a surface each, so this is what a frame on one of them starts with.
    bool makeCurrentOn(EGLSurface surface) const;

    // Said before a surface is destroyed: a surface that is current when it
    // goes leaves the context pointing at freed memory, so the context falls
    // back to surfaceless first.
    void surfaceIsGoing(EGLSurface surface) const;

    // Gives the context up, so the next thread to want it can take it: an
    // EGLContext is current on one thread at a time. What Device's
    // followMainThread() does, the shared Device being constructed by whichever
    // thread asked for it first and driven by the main one afterwards.
    void releaseCurrent() const;

    const GLCapabilities& getCapabilities() const { return capabilities; }

    // The one vertex array object every draw configures (D6), made on first
    // use: a core context has no default one, and a VAO belongs to the context
    // that made it, so there is exactly this one per Device.
    GLuint getVertexArray() const;

    // How many of its attribute arrays are enabled, so a draw disables the ones
    // its own layout does not name rather than counting from a fixed ceiling.
    int getEnabledVertexAttributes() const { return enabledAttributes; }
    void setEnabledVertexAttributes(int count) const { enabledAttributes = count; }

    // The display and config a surface is made on.
    EGLDisplay getDisplay() const { return display; }
    EGLContext getContext() const { return context; }

    // Null where the context was made with no config at all
    // (EGL_KHR_no_config_context), which is what lets a window surface be made
    // on whichever config the native window's own visual matches.
    EGLConfig getConfig() const { return config; }

    // The configs a window surface may be made on, best first: the context's
    // own where it has one, and every single-sampled window-capable RGB8
    // config otherwise, carrying a depth or stencil plane where the view asked
    // for one. Empty where nothing answers, which is what sends the view to a
    // companion framebuffer for the planes instead.
    //
    // A caller tries them in turn rather than taking the first: only the
    // native window knows which visual it was made with, and on X11 a config
    // that does not match it is refused.
    Vector<EGLConfig> windowConfigs(bool depth, bool stencil) const;

private:
    bool createContext();
    void probeCapabilities();

    mutable GLuint vertexArray = 0;
    mutable int enabledAttributes = 0;

    // The surface this context was last made current with, so a thread that
    // already has the context keeps whatever it was drawing into and a frame
    // on a view binds its own.
    mutable EGLSurface currentSurface = EGL_NO_SURFACE;

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLConfig config = nullptr;

    GLCapabilities capabilities;
};

// One integer attribute of a config, and zero where the display will not say:
// what a surface asks a config it is about to be made on - how deep its depth
// plane is, whether it carries a stencil one.
int glConfigAttribute(EGLDisplay display, EGLConfig config, EGLint attribute);

// The process-wide EGL display, opened on the first Device and closed with the
// last. False where libEGL is absent or no display answered.
bool glDisplayIsAvailable();

// Whether the EGL device the display sits on is a software one -
// EGL_MESA_device_software on the device EGL_EXT_device_query names. What D8's
// auto rule will weigh against Vulkan's own answer in stage 4; true where there
// is nothing to ask.
bool glDisplayIsSoftware();

// The GL spelling of a TextureFormat on a given context: the sized internal
// format, and the pair an upload is phrased in. An invalid GLFormat is a
// format this context cannot carry - a BC one with neither extension - which
// is refused at texture creation rather than stored as something else.
GLFormat glFormatFor(TextureFormat format, const GLCapabilities& capabilities);

// Swaps the red and blue channels of a block of 4-byte rows in place: the CPU
// half of the swapped-BGRA route, on the way in and on the way back out.
void glSwapRedAndBlue(std::byte* rows, int width, int height, int bytesPerRow);

// Drains glGetError and logs whatever it held, under EACP_GL_DEBUG=1 alone.
// Every error the driver has to offer is already reported by the KHR_debug
// messenger that flag also installs; this is for the driver that has no
// KHR_debug to install one on.
void glDrainErrors(const char* what);

// Whether that flag is set, so a caller can skip building the string it would
// have passed.
bool glDebugIsOn();

// Empties the error queue without logging or caring, so that the next
// glGetError answers for the call it is asking about rather than for whatever
// was left lying about. What a creation path that decides isValid() by asking
// needs, and the only place the queue is touched outside EACP_GL_DEBUG.
void glForgetErrors();
} // namespace eacp::GPU
