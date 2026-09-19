#include "GLContext-Linux.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Logging.h>

#include <dlfcn.h>

#include <mutex>

namespace eacp::GPU
{
namespace
{
bool glReadDebugFlag()
{
    return getEnvValue("EACP_GL_DEBUG") == "1";
}

bool glWantsES()
{
    return getEnvValue("EACP_GL_ES") == "1";
}

// The whole of the link to EGL: the library by name, and eglGetProcAddress out
// of it. Nothing else in the process dlopens or links one.
struct GLEglLibrary
{
    GLEglLibrary()
    {
        handle = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);

        if (handle == nullptr)
            handle = dlopen("libEGL.so", RTLD_LAZY | RTLD_LOCAL);

        if (handle == nullptr)
        {
            LOG("OpenGL: no libEGL to open (", dlerror(), ")");
            return;
        }

        getProcAddress = reinterpret_cast<PFNEGLGETPROCADDRESSPROC>(
            dlsym(handle, "eglGetProcAddress"));

        if (getProcAddress == nullptr)
            LOG("OpenGL: libEGL has no eglGetProcAddress");
    }

    void* handle = nullptr;
    PFNEGLGETPROCADDRESSPROC getProcAddress = nullptr;
};

const GLEglLibrary& glGetEglLibrary()
{
    static const GLEglLibrary library;

    return library;
}

// glad's loader shape. eglGetProcAddress answers for the core entry points too
// on every EGL 1.5 implementation, and dlsym is the fallback for the ones an
// older one would only hand out that way.
GLADapiproc glLoadEglSymbol(void*, const char* name)
{
    const auto& library = glGetEglLibrary();

    if (library.getProcAddress != nullptr)
        if (auto* found = library.getProcAddress(name); found != nullptr)
            return reinterpret_cast<GLADapiproc>(found);

    if (library.handle == nullptr)
        return nullptr;

    return reinterpret_cast<GLADapiproc>(dlsym(library.handle, name));
}

GLADapiproc glLoadGLSymbol(void*, const char* name)
{
    const auto& library = glGetEglLibrary();

    if (library.getProcAddress == nullptr)
        return nullptr;

    return reinterpret_cast<GLADapiproc>(library.getProcAddress(name));
}

bool glHasClientExtension(const char* extensions, std::string_view name)
{
    if (extensions == nullptr)
        return false;

    auto haystack = std::string_view {extensions};

    for (auto start = std::size_t {0}; start <= haystack.size();)
    {
        const auto end = haystack.find(' ', start);
        const auto token = haystack.substr(
            start, end == std::string_view::npos ? end : end - start);

        if (token == name)
            return true;

        if (end == std::string_view::npos)
            break;

        start = end + 1;
    }

    return false;
}

// Neither a Wayland compositor nor an X server to reach, which is the headless
// case the whole of stage 2 runs in.
bool glHasNoWindowSystem()
{
    if (getEnvValue("EACP_HEADLESS") == "1")
        return true;

    return getEnvValue("WAYLAND_DISPLAY").empty() && getEnvValue("DISPLAY").empty();
}

EGLDisplay glOpenDisplay()
{
    const auto* clientExtensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

    const auto hasSurfaceless =
        glHasClientExtension(clientExtensions, "EGL_MESA_platform_surfaceless");

    if (glHasNoWindowSystem() && hasSurfaceless && eglGetPlatformDisplay != nullptr)
    {
        auto display = eglGetPlatformDisplay(
            EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);

        if (display != EGL_NO_DISPLAY)
            return display;
    }

    return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

// The display and the refcount over it. Process-wide, as VulkanShared is: a
// second Device joins the one that is already up, and the last one out
// terminates it so a later Device opens a fresh one.
struct GLDisplayShared
{
    EGLDisplay retain()
    {
        const auto lock = std::scoped_lock {mutex};

        if (users == 0)
        {
            display = open();

            if (display == EGL_NO_DISPLAY)
                return EGL_NO_DISPLAY;
        }

        if (display == EGL_NO_DISPLAY)
            return EGL_NO_DISPLAY;

        ++users;
        return display;
    }

    void release()
    {
        const auto lock = std::scoped_lock {mutex};

        if (users == 0 || --users > 0)
            return;

        eglTerminate(display);
        display = EGL_NO_DISPLAY;
    }

    EGLDisplay peek()
    {
        const auto lock = std::scoped_lock {mutex};

        if (users > 0)
            return display;

        // Opened without a user, for the question D8 asks before it makes a
        // Device at all. Terminating it would cost the next Device the open
        // again, so it is left up and joined.
        if (probed == EGL_NO_DISPLAY)
            probed = open();

        return probed;
    }

    EGLDisplay open()
    {
        if (probed != EGL_NO_DISPLAY)
            return probed;

        if (glGetEglLibrary().getProcAddress == nullptr)
            return EGL_NO_DISPLAY;

        // Twice: once with no display for the client extensions the platform
        // choice reads, and again once there is one for the display's own.
        if (gladLoadEGLUserPtr(EGL_NO_DISPLAY, glLoadEglSymbol, nullptr) == 0)
        {
            LOG("OpenGL: EGL would not load");
            return EGL_NO_DISPLAY;
        }

        auto display = glOpenDisplay();

        if (display == EGL_NO_DISPLAY)
        {
            LOG("OpenGL: no EGL display answered");
            return EGL_NO_DISPLAY;
        }

        auto major = EGLint {0};
        auto minor = EGLint {0};

        if (eglInitialize(display, &major, &minor) == EGL_FALSE)
        {
            LOG("OpenGL: eglInitialize refused the display");
            return EGL_NO_DISPLAY;
        }

        gladLoadEGLUserPtr(display, glLoadEglSymbol, nullptr);

        probed = display;
        return display;
    }

    std::mutex mutex;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLDisplay probed = EGL_NO_DISPLAY;
    int users = 0;
};

GLDisplayShared& glGetDisplayShared()
{
    static GLDisplayShared shared;

    return shared;
}

// Which context this thread has current, so eglMakeCurrent runs only when the
// device changes. The context itself records which thread holds it, an
// EGLContext being current on one thread at a time.
thread_local const GLContext* glCurrentContext = nullptr;

void GLAD_API_PTR glDebugMessage(GLenum,
                                 GLenum type,
                                 GLuint,
                                 GLenum severity,
                                 GLsizei,
                                 const GLchar* message,
                                 const void*)
{
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION)
        return;

    LOG("OpenGL debug (type ", (int) type, "): ", message);
}

bool glHasExtension(std::string_view name)
{
    auto count = GLint {0};
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);

    for (auto i = 0; i < count; ++i)
    {
        const auto* extension = glGetStringi(GL_EXTENSIONS, (GLuint) i);

        if (extension != nullptr
            && name == reinterpret_cast<const char*>(extension))
            return true;
    }

    return false;
}

std::string glStringOf(GLenum name)
{
    const auto* value = glGetString(name);

    return value != nullptr ? std::string {reinterpret_cast<const char*>(value)}
                            : std::string {};
}

int glReadInteger(GLenum name, int fallback)
{
    auto value = GLint {fallback};
    glGetIntegerv(name, &value);

    // A driver that refuses the query leaves the value alone, and the error it
    // raised would otherwise sit in the queue until the next drain.
    while (glGetError() != GL_NO_ERROR)
        value = fallback;

    return (int) value;
}

// The versions asked for, highest first: a driver hands back what it can, and
// virgl refuses 4.6 and 4.3 while answering a 3.3 request with a 4.0 context.
constexpr int glCoreVersions[][2] = {
    {4, 6}, {4, 5}, {4, 3}, {4, 1}, {4, 0}, {3, 3}};

constexpr int glESVersions[][2] = {{3, 2}, {3, 1}, {3, 0}};
} // namespace

bool glDebugIsOn()
{
    static const auto on = glReadDebugFlag();

    return on;
}

void glDrainErrors(const char* what)
{
    if (!glDebugIsOn())
        return;

    for (auto error = glGetError(); error != GL_NO_ERROR; error = glGetError())
        LOG("OpenGL error 0x", (int) error, " at ", what);
}

void glForgetErrors()
{
    while (glGetError() != GL_NO_ERROR)
    {
    }
}

bool glDisplayIsAvailable()
{
    return glGetDisplayShared().peek() != EGL_NO_DISPLAY;
}

bool glDisplayIsSoftware()
{
    auto display = glGetDisplayShared().peek();

    if (display == EGL_NO_DISPLAY || eglQueryDisplayAttribEXT == nullptr
        || eglQueryDeviceStringEXT == nullptr)
        return true;

    auto attribute = EGLAttrib {0};

    if (eglQueryDisplayAttribEXT(display, EGL_DEVICE_EXT, &attribute) == EGL_FALSE)
        return true;

    const auto* extensions = eglQueryDeviceStringEXT(
        reinterpret_cast<EGLDeviceEXT>(attribute), EGL_EXTENSIONS);

    return glHasClientExtension(extensions, "EGL_MESA_device_software");
}

GLContext::GLContext()
{
    display = glGetDisplayShared().retain();

    if (display == EGL_NO_DISPLAY)
        return;

    if (!createContext())
    {
        glGetDisplayShared().release();
        display = EGL_NO_DISPLAY;
        return;
    }

    makeCurrent();
    probeCapabilities();
}

GLContext::~GLContext()
{
    if (display == EGL_NO_DISPLAY)
        return;

    if (context != EGL_NO_CONTEXT)
    {
        if (vertexArray != 0)
        {
            makeCurrent();
            glDeleteVertexArrays(1, &vertexArray);
            vertexArray = 0;
        }

        releaseCurrent();
        eglDestroyContext(display, context);
        context = EGL_NO_CONTEXT;
    }

    glGetDisplayShared().release();
    display = EGL_NO_DISPLAY;
}

bool GLContext::createContext()
{
    const auto es = glWantsES();

    if (eglBindAPI(es ? EGL_OPENGL_ES_API : EGL_OPENGL_API) == EGL_FALSE)
    {
        LOG("OpenGL: EGL offers no ", es ? "ES" : "desktop GL", " API");
        return false;
    }

    const auto* extensions = eglQueryString(display, EGL_EXTENSIONS);

    // No config at all where the display offers it, so the context is
    // compatible with whatever surface stage 3 later makes; a window-capable
    // RGBA8 config otherwise, for the same reason.
    if (!glHasClientExtension(extensions, "EGL_KHR_no_config_context"))
    {
        const EGLint attributes[] = {EGL_SURFACE_TYPE,
                                     EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                                     EGL_RENDERABLE_TYPE,
                                     es ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT,
                                     EGL_RED_SIZE,
                                     8,
                                     EGL_GREEN_SIZE,
                                     8,
                                     EGL_BLUE_SIZE,
                                     8,
                                     EGL_ALPHA_SIZE,
                                     8,
                                     EGL_NONE};

        auto found = EGLint {0};

        if (eglChooseConfig(display, attributes, &config, 1, &found) == EGL_FALSE
            || found == 0)
        {
            LOG("OpenGL: the display offers no RGBA8 config");
            return false;
        }
    }

    const auto tryVersion = [this, es](int major, int minor)
    {
        EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION,
                               major,
                               EGL_CONTEXT_MINOR_VERSION,
                               minor,
                               EGL_NONE,
                               EGL_NONE,
                               EGL_NONE};

        if (!es)
        {
            attributes[4] = EGL_CONTEXT_OPENGL_PROFILE_MASK;
            attributes[5] = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT;
        }

        context = eglCreateContext(display, config, EGL_NO_CONTEXT, attributes);

        return context != EGL_NO_CONTEXT;
    };

    if (es)
    {
        for (const auto& version: glESVersions)
            if (tryVersion(version[0], version[1]))
                return true;
    }
    else
    {
        for (const auto& version: glCoreVersions)
            if (tryVersion(version[0], version[1]))
                return true;
    }

    LOG("OpenGL: no ",
        es ? "ES 3.0" : "3.3 core",
        " context or better came up on this display");

    return false;
}

void GLContext::makeCurrent() const
{
    if (glCurrentContext == this || context == EGL_NO_CONTEXT)
        return;

    // Surfaceless: EGL_KHR_surfaceless_context, which Mesa has had for a
    // decade, and the only thing a headless render needs.
    if (eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context)
        == EGL_FALSE)
    {
        LOG("OpenGL: the context would not be made current on this thread");
        return;
    }

    glCurrentContext = this;
}

void GLContext::releaseCurrent() const
{
    if (glCurrentContext != this)
        return;

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    glCurrentContext = nullptr;
}

GLuint GLContext::getVertexArray() const
{
    if (vertexArray == 0)
        glGenVertexArrays(1, &vertexArray);

    return vertexArray;
}

void GLContext::probeCapabilities()
{
    const auto es = glWantsES();

    if ((es ? gladLoadGLES2UserPtr(glLoadGLSymbol, nullptr)
            : gladLoadGLUserPtr(glLoadGLSymbol, nullptr))
        == 0)
    {
        LOG("OpenGL: the context came up but its entry points would not load");

        eglDestroyContext(display, context);
        context = EGL_NO_CONTEXT;
        glCurrentContext = nullptr;
        return;
    }

    auto& caps = capabilities;

    caps.isES = es;
    caps.renderer = glStringOf(GL_RENDERER);
    caps.vendor = glStringOf(GL_VENDOR);
    caps.versionString = glStringOf(GL_VERSION);
    caps.shadingLanguageVersion = glStringOf(GL_SHADING_LANGUAGE_VERSION);

    const auto major = glReadInteger(GL_MAJOR_VERSION, es ? 3 : 3);
    const auto minor = glReadInteger(GL_MINOR_VERSION, es ? 0 : 3);

    caps.version = major * 100 + minor * 10;

    caps.computeShaders =
        es ? caps.version >= 310 : (caps.version >= 430 || glHasExtension(
                                        "GL_ARB_compute_shader"));
    caps.storageBuffers =
        es ? caps.version >= 310
           : (caps.version >= 430
              || glHasExtension("GL_ARB_shader_storage_buffer_object"));
    caps.imageLoadStore =
        es ? caps.version >= 310
           : (caps.version >= 420
              || glHasExtension("GL_ARB_shader_image_load_store"));

    caps.bufferStorage = glad_glBufferStorage != nullptr;
    caps.bufferStorageIsEXT = false;

    if (!caps.bufferStorage && glad_glBufferStorageEXT != nullptr)
    {
        caps.bufferStorage = true;
        caps.bufferStorageIsEXT = true;
    }

    caps.textureStorage = glad_glTexStorage2D != nullptr;

    caps.timerQuery = !es && glad_glQueryCounter != nullptr;
    caps.timerQueryIsEXT = false;

    if (!caps.timerQuery && glad_glQueryCounterEXT != nullptr
        && glHasExtension("GL_EXT_disjoint_timer_query"))
    {
        caps.timerQuery = true;
        caps.timerQueryIsEXT = true;
    }

    caps.explicitBindings =
        es ? caps.version >= 310
           : (caps.version >= 420
              || glHasExtension("GL_ARB_shading_language_420pack"));
    caps.separateShaderObjects =
        es ? true
           : (caps.version >= 410
              || glHasExtension("GL_ARB_separate_shader_objects"));

    caps.bptc = caps.version >= 420 && !es;
    caps.bptc = caps.bptc || glHasExtension("GL_ARB_texture_compression_bptc")
                || glHasExtension("GL_EXT_texture_compression_bptc");
    caps.s3tc = glHasExtension("GL_EXT_texture_compression_s3tc");

    caps.clipControl = glad_glClipControl != nullptr;
    caps.clipControlIsEXT = false;

    if (!caps.clipControl && glad_glClipControlEXT != nullptr
        && glHasExtension("GL_EXT_clip_control"))
    {
        caps.clipControl = true;
        caps.clipControlIsEXT = true;
    }
    caps.debugOutput = glad_glDebugMessageCallback != nullptr;

    caps.bgraFormat = !es;
    caps.getTexImage = glad_glGetTexImage != nullptr;
    caps.getBufferSubData = glad_glGetBufferSubData != nullptr;

    caps.floatRenderTargets = !es || glHasExtension("GL_EXT_color_buffer_float");
    caps.halfFloatRenderTargets =
        caps.floatRenderTargets || glHasExtension("GL_EXT_color_buffer_half_float");

    caps.maxSamples = glReadInteger(GL_MAX_SAMPLES, 1);
    caps.maxTextureSize = glReadInteger(GL_MAX_TEXTURE_SIZE, 0);
    caps.uniformBufferOffsetAlignment =
        glReadInteger(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, 4);

    if (caps.storageBuffers)
        caps.storageBufferOffsetAlignment =
            glReadInteger(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, 4);

    if (caps.computeShaders)
        caps.maxThreadgroupMemory =
            glReadInteger(GL_MAX_COMPUTE_SHARED_MEMORY_SIZE, 0);

    // Clip space leaves depth in [0, 1] on the other three backends and in
    // [-1, 1] here, so a context that can be told takes the first: a depth read
    // back off a target is then the number the vertex stage wrote. One that
    // cannot - virgl - orders fragments identically and reads back the far half
    // of the range, which is stage 4's quirk rather than a second dialect.
    if (caps.clipControlIsEXT)
        glClipControlEXT(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    else if (caps.clipControl)
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);

    // Filtering across a cube's edges, which ES and the other three backends do
    // always and desktop GL only when asked.
    if (!es)
        glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    if (glDebugIsOn() && caps.debugOutput)
    {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(glDebugMessage, nullptr);
    }

    glDrainErrors("capability probe");
}

GLFormat glFormatFor(TextureFormat format, const GLCapabilities& capabilities)
{
    auto result = GLFormat {};

    switch (format)
    {
        case TextureFormat::RGBA8Unorm:
            result.internalFormat = GL_RGBA8;
            result.format = GL_RGBA;
            result.type = GL_UNSIGNED_BYTE;
            return result;

        // ES has no GL_BGRA internal format, so the texels are stored RGBA and
        // every row is swapped on the way through - on the way back out too,
        // so what is stored is what every other backend stores.
        case TextureFormat::BGRA8Unorm:
            result.internalFormat = GL_RGBA8;
            result.type = GL_UNSIGNED_BYTE;

            if (capabilities.bgraFormat)
            {
                result.format = GL_BGRA;
                return result;
            }

            result.format = GL_RGBA;
            result.swappedBGRA = true;
            return result;

        case TextureFormat::R8Unorm:
            result.internalFormat = GL_R8;
            result.format = GL_RED;
            result.type = GL_UNSIGNED_BYTE;
            return result;

        case TextureFormat::RG8Unorm:
            result.internalFormat = GL_RG8;
            result.format = GL_RG;
            result.type = GL_UNSIGNED_BYTE;
            return result;

        case TextureFormat::RGBA16Float:
            result.internalFormat = GL_RGBA16F;
            result.format = GL_RGBA;
            result.type = GL_HALF_FLOAT;
            return result;

        case TextureFormat::RGBA32Float:
            result.internalFormat = GL_RGBA32F;
            result.format = GL_RGBA;
            result.type = GL_FLOAT;
            return result;

        case TextureFormat::R32Float:
            result.internalFormat = GL_R32F;
            result.format = GL_RED;
            result.type = GL_FLOAT;
            return result;

        case TextureFormat::BC1RGBA:
            result.compressed = true;
            result.internalFormat =
                capabilities.s3tc ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT : 0;
            return result;

        case TextureFormat::BC2RGBA:
            result.compressed = true;
            result.internalFormat =
                capabilities.s3tc ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT : 0;
            return result;

        case TextureFormat::BC3RGBA:
            result.compressed = true;
            result.internalFormat =
                capabilities.s3tc ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT : 0;
            return result;

        case TextureFormat::BC7RGBA:
            result.compressed = true;
            result.internalFormat =
                capabilities.bptc ? GL_COMPRESSED_RGBA_BPTC_UNORM : 0;
            return result;
    }

    return result;
}

void glSwapRedAndBlue(std::byte* rows, int width, int height, int bytesPerRow)
{
    for (auto row = 0; row < height; ++row)
    {
        auto* pixels = rows + static_cast<std::size_t>(row)
                                  * static_cast<std::size_t>(bytesPerRow);

        for (auto x = 0; x < width; ++x)
        {
            auto* texel = pixels + static_cast<std::size_t>(x) * 4;
            const auto red = texel[0];

            texel[0] = texel[2];
            texel[2] = red;
        }
    }
}
} // namespace eacp::GPU
