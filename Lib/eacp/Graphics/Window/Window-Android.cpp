#include "Android.h"
#include "AndroidEnvironment-Android.h"
#include "Window.h"

#include "../Graphics/Keyboard.h"
#include "LinuxWindowSystem-Linux.h"
#include "../View/AndroidViewSurface-Android.h"

#include <eacp/Core/Android/Jni.h>
#include <eacp/Core/App/App.h>
#include <eacp/Core/Threads/EventLoop-Android.h>
#include <eacp/Core/Threads/Timer.h>
#include <eacp/Core/Utils/FilePath-Android.h>

#include <android/configuration.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <optional>
#include <string>

namespace eacp::Graphics
{
namespace
{
constexpr auto nanosecondsPerSecond = 1e9;
constexpr auto insetsRefreshHz = 4;

// Before the native window arrives.
const auto androidInitialContentSize = Point {640.f, 400.f};

// The activity as the glue reports it, outliving any one Window: the native
// window can arrive before the app has built one.
struct AndroidWindow;

struct AndroidActivity
{
    android_app* app = nullptr;
    AndroidWindow* window = nullptr;
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

struct AndroidInsetsJava
{
    void resolve(Jni::Lookup& lookup)
    {
        auto* types = lookup.findClass("android/view/WindowInsets$Type");
        auto* windowInsets = lookup.findClass("android/view/WindowInsets");
        auto* insets = lookup.findClass("android/graphics/Insets");

        auto* controller = lookup.findClass("android/view/WindowInsetsController");

        auto type = [&](const char* name)
        {
            auto method = lookup.staticMethod(types, name, "()I");
            return method != nullptr ? lookup.env->CallStaticIntMethod(types, method)
                                     : 0;
        };

        ime = type("ime");
        mask = type("systemBars") | type("displayCutout") | ime;

        getInsets =
            lookup.method(windowInsets, "getInsets", "(I)Landroid/graphics/Insets;");
        left = lookup.field(insets, "left", "I");
        top = lookup.field(insets, "top", "I");
        right = lookup.field(insets, "right", "I");
        bottom = lookup.field(insets, "bottom", "I");
        show = lookup.method(controller, "show", "(I)V");
    }

    jint mask = 0;
    jint ime = 0;
    jmethodID getInsets = nullptr;
    jmethodID show = nullptr;
    jfieldID left = nullptr;
    jfieldID top = nullptr;
    jfieldID right = nullptr;
    jfieldID bottom = nullptr;
};

struct AndroidKeyJava
{
    void resolve(Jni::Lookup& lookup)
    {
        keyEvent = lookup.findClass("android/view/KeyEvent");
        init = lookup.method(keyEvent, "<init>", "(II)V");
        getUnicodeChar = lookup.method(keyEvent, "getUnicodeChar", "(I)I");
    }

    jclass keyEvent = nullptr;
    jmethodID init = nullptr;
    jmethodID getUnicodeChar = nullptr;
};

// What the key types, from its key map; the NDK has no call for it.
std::string androidKeyCharacters(const AInputEvent* event)
{
    auto* env = Jni::currentEnv();
    const auto* java =
        env != nullptr ? Jni::resolveOnce<AndroidKeyJava>(env) : nullptr;

    if (java == nullptr)
        return {};

    auto frame = Jni::LocalFrame {env};
    auto* key = env->NewObject(java->keyEvent,
                               java->init,
                               AKeyEvent_getAction(event),
                               AKeyEvent_getKeyCode(event));
    auto character = Jni::failed(env)
                         ? 0
                         : env->CallIntMethod(key,
                                              java->getUnicodeChar,
                                              AKeyEvent_getMetaState(event));

    if (Jni::failed(env) || character <= 0)
        return {};

    return Strings::narrow(std::wstring(1, (wchar_t) character));
}

// The system bars', the cutout's and the keyboard's insets in pixels, read over JNI: the glue's
// content rect covers the whole window once an app is edge to edge, which
// every app targeting API 35 is. Empty before the decor view is attached.
std::optional<ARect> androidSystemInsets(ANativeActivity* activity)
{
    auto* env = Jni::currentEnv();
    const auto* java =
        env != nullptr ? Jni::resolveOnce<AndroidInsetsJava>(env) : nullptr;

    if (java == nullptr || activity == nullptr)
        return std::nullopt;

    auto frame = Jni::LocalFrame {env};
    auto* window = Jni::callObject(
        env, activity->clazz, "getWindow", "()Landroid/view/Window;");
    auto* decor =
        Jni::callObject(env, window, "getDecorView", "()Landroid/view/View;");
    auto* rootInsets = Jni::callObject(
        env, decor, "getRootWindowInsets", "()Landroid/view/WindowInsets;");

    if (rootInsets == nullptr)
        return std::nullopt;

    auto* insets = env->CallObjectMethod(rootInsets, java->getInsets, java->mask);

    if (Jni::failed(env) || insets == nullptr)
        return std::nullopt;

    return ARect {env->GetIntField(insets, java->left),
                  env->GetIntField(insets, java->top),
                  env->GetIntField(insets, java->right),
                  env->GetIntField(insets, java->bottom)};
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

        refreshInsets();

        return contentSize.x != oldSize.x || contentSize.y != oldSize.y;
    }

    void refreshInsets()
    {
        auto* app = androidActivity().app;

        if (app == nullptr || nativeWindow == nullptr)
            return;

        if (auto system = androidSystemInsets(app->activity))
            readSystemInsets(*system);
        else
            readInsets(app->contentRect);

        if (contentView != nullptr)
            contentView->setSafeAreaInsets(insets);
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

    // No command comes when the on-screen keyboard shows or hides.
    Threads::Timer insetsTimer {[this] { refreshInsets(); }, insetsRefreshHz};
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

uint16_t androidKeyCode(int32_t code)
{
    switch (code)
    {
        case AKEYCODE_BACK:
            return KeyCode::Escape;
        case AKEYCODE_DEL:
            return KeyCode::Delete;
        case AKEYCODE_ENTER:
            return KeyCode::Return;
        default:
            return KeyCode::Unknown;
    }
}

int32_t androidHandleKey(AInputEvent* event)
{
    auto key = KeyEvent {};
    key.keyCode = androidKeyCode(AKeyEvent_getKeyCode(event));

    if (key.keyCode == KeyCode::Unknown)
        key.characters = androidKeyCharacters(event);

    if (key.keyCode == KeyCode::Unknown && key.characters.empty())
        return 0;

    auto* window = androidActivity().window;

    if (window == nullptr || window->contentView == nullptr)
        return 1;

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
            Apps::Detail::setSuspended(false);
            [[fallthrough]];
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
        case APP_CMD_CONTENT_RECT_CHANGED:
            if (window != nullptr)
                window->surfaceChanged(app);
            break;

        case APP_CMD_TERM_WINDOW:
            if (window != nullptr)
                window->nativeWindowLost();
            Apps::Detail::setSuspended(true);
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
        case APP_CMD_PAUSE:
            Apps::Detail::setSuspended(command == APP_CMD_PAUSE);
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

// ANativeActivity_showSoftInput is ignored since Android 12: NativeActivity's
// view is not one the input method serves.
void linuxViewFocused()
{
    auto* app = androidActivity().app;
    auto* env = Jni::currentEnv();
    const auto* java =
        env != nullptr ? Jni::resolveOnce<AndroidInsetsJava>(env) : nullptr;

    if (app == nullptr || java == nullptr)
        return;

    auto frame = Jni::LocalFrame {env};
    auto* window = Jni::callObject(
        env, app->activity->clazz, "getWindow", "()Landroid/view/Window;");
    auto* controller = Jni::callObject(env,
                                       window,
                                       "getInsetsController",
                                       "()Landroid/view/WindowInsetsController;");

    if (controller != nullptr)
    {
        env->CallVoidMethod(controller, java->show, java->ime);
        Jni::failed(env);
    }
}

namespace Android
{
android_app* getApp()
{
    return androidActivity().app;
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
    eacp::Jni::setJavaVM(app->activity->vm);

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
