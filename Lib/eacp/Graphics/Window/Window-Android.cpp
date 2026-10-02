#include "Android.h"
#include "AndroidEnvironment-Android.h"
#include "Window.h"

#include "../Graphics/Keyboard.h"
#include "LinuxWindowSystem-Linux.h"
#include "../View/AndroidViewSurface-Android.h"

#include <eacp/Core/App/App.h>
#include <eacp/Core/Threads/EventLoop-Android.h>
#include <eacp/Core/Utils/FilePath-Android.h>

#include <android/configuration.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <optional>
#include <string>

namespace eacp::Graphics
{
namespace
{
constexpr auto androidInsetsLocalReferences = 16;
constexpr auto nanosecondsPerSecond = 1e9;

// Before the native window arrives.
const auto androidInitialContentSize = Point {640.f, 400.f};

// The activity as the glue reports it, outliving any one Window: the native
// window can arrive before the app has built one.
struct AndroidWindow;

struct AndroidActivity
{
    android_app* app = nullptr;
    AndroidWindow* window = nullptr;

    std::function<void(bool)> lifecycleHandler = [](bool) {};
};

AndroidActivity& androidActivity()
{
    static auto activity = AndroidActivity {};
    return activity;
}

float androidBackingScale(android_app* app)
{
    if (app == nullptr || app->config == nullptr)
        return 1.f;

    auto density = AConfiguration_getDensity(app->config);

    if (density <= 0 || density == ACONFIGURATION_DENSITY_ANY
        || density == ACONFIGURATION_DENSITY_NONE)
        return 1.f;

    return (float) density / (float) ACONFIGURATION_DENSITY_MEDIUM;
}

bool androidJavaFailed(JNIEnv* env)
{
    if (!env->ExceptionCheck())
        return false;

    env->ExceptionClear();
    return true;
}

jobject androidCallObject(JNIEnv* env,
                          jobject target,
                          const char* name,
                          const char* signature)
{
    if (target == nullptr)
        return nullptr;

    auto method = env->GetMethodID(env->GetObjectClass(target), name, signature);

    if (androidJavaFailed(env))
        return nullptr;

    auto* result = env->CallObjectMethod(target, method);
    return androidJavaFailed(env) ? nullptr : result;
}

jint androidInsetsType(JNIEnv* env, jclass types, const char* name)
{
    auto method = env->GetStaticMethodID(types, name, "()I");

    if (androidJavaFailed(env))
        return 0;

    auto type = env->CallStaticIntMethod(types, method);
    return androidJavaFailed(env) ? 0 : type;
}

std::optional<jint> androidIntField(JNIEnv* env, jobject target, const char* name)
{
    auto field = env->GetFieldID(env->GetObjectClass(target), name, "I");

    if (androidJavaFailed(env))
        return std::nullopt;

    return env->GetIntField(target, field);
}

std::optional<ARect> androidReadSystemInsets(JNIEnv* env, jobject activity)
{
    auto* window =
        androidCallObject(env, activity, "getWindow", "()Landroid/view/Window;");
    auto* decor =
        androidCallObject(env, window, "getDecorView", "()Landroid/view/View;");
    auto* rootInsets = androidCallObject(
        env, decor, "getRootWindowInsets", "()Landroid/view/WindowInsets;");

    if (rootInsets == nullptr)
        return std::nullopt;

    auto* types = env->FindClass("android/view/WindowInsets$Type");

    if (androidJavaFailed(env))
        return std::nullopt;

    auto mask = androidInsetsType(env, types, "systemBars")
                | androidInsetsType(env, types, "displayCutout");
    auto getInsets = env->GetMethodID(env->GetObjectClass(rootInsets),
                                      "getInsets",
                                      "(I)Landroid/graphics/Insets;");

    if (androidJavaFailed(env))
        return std::nullopt;

    auto* insets = env->CallObjectMethod(rootInsets, getInsets, mask);

    if (androidJavaFailed(env) || insets == nullptr)
        return std::nullopt;

    auto left = androidIntField(env, insets, "left");
    auto top = androidIntField(env, insets, "top");
    auto right = androidIntField(env, insets, "right");
    auto bottom = androidIntField(env, insets, "bottom");

    if (!left || !top || !right || !bottom)
        return std::nullopt;

    return ARect {*left, *top, *right, *bottom};
}

// The system bars' and the cutout's insets in pixels, read over JNI: the glue's
// content rect covers the whole window once an app is edge to edge, which
// every app targeting API 35 is. Empty before the decor view is attached.
std::optional<ARect> androidSystemInsets(ANativeActivity* activity)
{
    if (activity == nullptr || activity->vm == nullptr)
        return std::nullopt;

    auto* env = static_cast<JNIEnv*>(nullptr);
    auto attached = false;

    if (activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6)
        != JNI_OK)
    {
        if (activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
            return std::nullopt;

        attached = true;
    }

    auto result = std::optional<ARect> {};

    if (env->PushLocalFrame(androidInsetsLocalReferences) == 0)
    {
        result = androidReadSystemInsets(env, activity->clazz);
        env->PopLocalFrame(nullptr);
    }
    else
    {
        androidJavaFailed(env);
    }

    if (attached)
        activity->vm->DetachCurrentThread();

    return result;
}

void androidHandleCommand(android_app* app, int32_t command);
int32_t androidHandleInput(android_app* app, AInputEvent* event);

struct AndroidWindow : AndroidWindowSurface
{
    AndroidWindow(const WindowOptions& optionsToUse, WindowEvents& eventsToUse)
        : quitCallback(optionsToUse.effectiveOnQuit())
        , onResize(optionsToUse.onResize)
        , events(&eventsToUse)
    {
        contentSize = androidInitialContentSize;
        viewSurfaces = makeAndroidViewSurfaceBackend(*this);

        auto& activity = androidActivity();
        activity.window = this;

        refresh(activity.app);
    }

    ~AndroidWindow()
    {
        if (contentView != nullptr)
            linuxUnbindWindowFromContentView(*contentView);

        auto& activity = androidActivity();

        if (activity.window == this)
            activity.window = nullptr;
    }

    bool refresh(android_app* app)
    {
        auto* current = app != nullptr ? app->window : nullptr;
        auto oldSize = contentSize;

        nativeWindow = current;
        scale = androidBackingScale(app);
        mapped = nativeWindow != nullptr;
        nativeSurface = {};

        if (nativeWindow != nullptr)
            nativeSurface = {
                NativeSurfaceHandle::Kind::Android, nullptr, nativeWindow, 0};

        if (nativeWindow != nullptr)
        {
            pixelWidth = ANativeWindow_getWidth(nativeWindow);
            pixelHeight = ANativeWindow_getHeight(nativeWindow);
            contentSize = {(float) pixelWidth / scale, (float) pixelHeight / scale};
        }
        else
        {
            pixelWidth = 0;
            pixelHeight = 0;
        }

        if (app != nullptr && nativeWindow != nullptr)
        {
            if (auto system = androidSystemInsets(app->activity))
                readSystemInsets(*system);
            else
                readInsets(app->contentRect);

            if (contentView != nullptr)
                contentView->setSafeAreaInsets(insets);
        }

        return contentSize.x != oldSize.x || contentSize.y != oldSize.y;
    }

    void readSystemInsets(const ARect& pixels)
    {
        insets.top = (float) pixels.top / scale;
        insets.left = (float) pixels.left / scale;
        insets.bottom = (float) pixels.bottom / scale;
        insets.right = (float) pixels.right / scale;
    }

    void readInsets(const ARect& rect)
    {
        if (rect.right <= rect.left || rect.bottom <= rect.top)
        {
            insets = {};
            return;
        }

        insets.top = (float) rect.top / scale;
        insets.left = (float) rect.left / scale;
        insets.bottom = (float) std::max(pixelHeight - rect.bottom, 0) / scale;
        insets.right = (float) std::max(pixelWidth - rect.right, 0) / scale;
    }

    void surfaceChanged(android_app* app)
    {
        auto resized = refresh(app);

        if (contentView == nullptr)
            return;

        if (resized)
            layOutContent();

        linuxWindowSurfaceStateChanged(*contentView);
    }

    // The glue still holds the window here; the views must let go of it.
    void nativeWindowLost()
    {
        nativeWindow = nullptr;
        nativeSurface = {};
        mapped = false;

        if (contentView != nullptr)
            linuxWindowSurfaceStateChanged(*contentView);
    }

    void layOutContent()
    {
        if (contentView == nullptr)
            return;

        contentView->setBounds({0.f, 0.f, contentSize.x, contentSize.y});

        if (onResize)
            onResize((int) contentSize.x, (int) contentSize.y);
    }

    void setContentView(View* view)
    {
        if (contentView != nullptr)
            linuxUnbindWindowFromContentView(*contentView);

        contentView = view;

        if (contentView == nullptr)
            return;

        contentView->setSafeAreaInsets(insets);
        layOutContent();
        linuxBindWindowToContentView(*contentView, *this);
    }

    Callback quitCallback;
    ResizeCallback onResize;
    WindowEvents* events;

    Insets insets;
    bool focused = false;
};

// Pointer ids are Android's plus one, so a finger is never 0, as on iOS.
void androidDispatchPointer(AndroidWindow* window,
                            TouchPhase phase,
                            const AInputEvent* event,
                            size_t index)
{
    if (window == nullptr || window->contentView == nullptr)
        return;

    auto touch = TouchEvent {};
    touch.id = (int) AMotionEvent_getPointerId(event, index) + 1;
    touch.phase = phase;
    touch.pos = {AMotionEvent_getX(event, index) / window->scale,
                 AMotionEvent_getY(event, index) / window->scale};
    touch.pressure = AMotionEvent_getPressure(event, index);
    touch.timestamp =
        (double) AMotionEvent_getEventTime(event) / nanosecondsPerSecond;

    window->contentView->dispatchTouchEvent(touch);
}

int32_t androidHandleMotion(AInputEvent* event)
{
    auto* window = androidActivity().window;
    const auto action = AMotionEvent_getAction(event);
    const auto masked = action & AMOTION_EVENT_ACTION_MASK;
    const auto index = (size_t) ((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                                 >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);

    switch (masked)
    {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            androidDispatchPointer(window, TouchPhase::Began, event, index);
            return 1;

        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP:
            androidDispatchPointer(window, TouchPhase::Ended, event, index);
            return 1;

        case AMOTION_EVENT_ACTION_CANCEL:
            for (auto i = size_t {0}; i < AMotionEvent_getPointerCount(event); ++i)
                androidDispatchPointer(window, TouchPhase::Cancelled, event, i);
            return 1;

        case AMOTION_EVENT_ACTION_MOVE:
            for (auto i = size_t {0}; i < AMotionEvent_getPointerCount(event); ++i)
                androidDispatchPointer(window, TouchPhase::Moved, event, i);
            return 1;

        default:
            return 0;
    }
}

int32_t androidHandleKey(AInputEvent* event)
{
    if (AKeyEvent_getKeyCode(event) != AKEYCODE_BACK)
        return 0;

    auto* window = androidActivity().window;

    if (window == nullptr || window->contentView == nullptr)
        return 1;

    auto key = KeyEvent {};
    key.keyCode = KeyCode::Escape;
    key.timestamp = (double) AKeyEvent_getEventTime(event) / nanosecondsPerSecond;

    if (AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_DOWN)
    {
        key.type = KeyEventType::Down;
        key.isRepeat = AKeyEvent_getRepeatCount(event) > 0;
        window->contentView->keyDown(key);
    }
    else if (AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_UP)
    {
        key.type = KeyEventType::Up;
        window->contentView->keyUp(key);
    }

    return 1;
}

int32_t androidHandleInput(android_app*, AInputEvent* event)
{
    switch (AInputEvent_getType(event))
    {
        case AINPUT_EVENT_TYPE_MOTION:
            return androidHandleMotion(event);

        case AINPUT_EVENT_TYPE_KEY:
            return androidHandleKey(event);

        default:
            return 0;
    }
}

// Runs between the glue's pre- and post-command steps, so a TERM_WINDOW has
// dropped the swapchain before the glue lets the surface go.
void androidHandleCommand(android_app* app, int32_t command)
{
    auto& activity = androidActivity();
    auto* window = activity.window;

    switch (command)
    {
        case APP_CMD_INIT_WINDOW:
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
        case APP_CMD_CONTENT_RECT_CHANGED:
            if (window != nullptr)
                window->surfaceChanged(app);
            break;

        case APP_CMD_TERM_WINDOW:
            if (window != nullptr)
                window->nativeWindowLost();
            break;

        case APP_CMD_GAINED_FOCUS:
        case APP_CMD_LOST_FOCUS:
            if (window != nullptr)
            {
                window->focused = command == APP_CMD_GAINED_FOCUS;
                window->events->onActivationChanged(window->focused);
            }
            break;

        case APP_CMD_RESUME:
            activity.lifecycleHandler(true);
            break;

        case APP_CMD_PAUSE:
            activity.lifecycleHandler(false);
            break;

        case APP_CMD_DESTROY:
            Apps::quit();
            break;

        default:
            break;
    }
}

void androidHandleLooperEvent(int ident, void* data)
{
    auto* app = androidActivity().app;

    if (app == nullptr || data == nullptr)
        return;

    if (ident == LOOPER_ID_MAIN || ident == LOOPER_ID_INPUT)
    {
        auto* source = static_cast<android_poll_source*>(data);
        source->process(app, source);
    }
}
} // namespace

struct Window::Native : AndroidWindow
{
    using AndroidWindow::AndroidWindow;
};

LinuxWindowSurface* linuxPointerWindow()
{
    return nullptr;
}

Point linuxPointerPosition()
{
    return {};
}

void linuxRefreshCursor() {}

namespace Android
{
android_app* getApp()
{
    return androidActivity().app;
}

void setLifecycleHandler(std::function<void(bool resumed)> handler)
{
    androidActivity().lifecycleHandler =
        handler ? std::move(handler) : std::function<void(bool)> {[](bool) {}};
}
} // namespace Android

Window::Window(const WindowOptions& optionsToUse)
    : options(optionsToUse)
    , impl(optionsToUse, events)
{
}

Window::~Window() = default;

void Window::setTitle(const std::string&) {}

void* Window::getHandle()
{
    return impl->nativeWindow;
}

void* Window::getContentViewHandle()
{
    return impl->contentView != nullptr ? impl->contentView->getHandle() : nullptr;
}

void Window::setContentView(View& view)
{
    contentLink.attach(&view, this);
    impl->setContentView(&view);
}

void Window::toFront() {}

void Window::setVisible(bool) {}

bool Window::isVisible()
{
    return impl->nativeWindow != nullptr;
}

Point Window::getPosition() const
{
    return {};
}

void Window::setPosition(Point) {}

Point Window::getSize() const
{
    return impl->contentSize;
}

void Window::setSize(Point) {}

void Window::minimize() {}

void Window::toggleMaximize() {}

void Window::setMouseLocked(bool) {}

bool Window::isMouseLocked() const
{
    return false;
}

bool Window::isKeyPressed(uint16_t virtualKeyCode) const
{
    return Keyboard::isKeyPressed(virtualKeyCode);
}

bool Window::isShiftPressed() const
{
    return Keyboard::isShiftPressed();
}

bool Window::isControlPressed() const
{
    return Keyboard::isControlPressed();
}

bool Window::isAltPressed() const
{
    return Keyboard::isAltPressed();
}

bool Window::isCommandPressed() const
{
    return Keyboard::isCommandPressed();
}

ModifierKeys Window::getModifiers() const
{
    return Keyboard::getModifiers();
}

} // namespace eacp::Graphics

// Called by android_main (AndroidMain-Android.c) around the app's main().
extern "C" void eacpAndroidStart(android_app* app)
{
    using namespace eacp::Graphics;

    androidActivity().app = app;

    if (app->activity != nullptr && app->activity->internalDataPath != nullptr)
        eacp::setAndroidDataDirectory(app->activity->internalDataPath);

    importAndroidEnvironment(app->activity);

    app->onAppCmd = androidHandleCommand;
    app->onInputEvent = androidHandleInput;

    eacp::Threads::setLooperEventHandler(androidHandleLooperEvent);
}

extern "C" void eacpAndroidFinish(android_app* app)
{
    ANativeActivity_finish(app->activity);
    eacp::Graphics::androidActivity().app = nullptr;
}
