#include "Android.h"
#include "AndroidEnvironment-Android.h"
#include "Window.h"

#include "../Graphics/Keyboard-Android.h"
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

#include <cmath>
#include <optional>
#include <string>

namespace eacp::Graphics
{
namespace
{
constexpr auto nanosecondsPerSecond = 1e9;
constexpr auto insetsRefreshHz = 4;

// The Linux backends' double-click rule, so a double tap counts the same.
constexpr auto doubleTapIntervalSeconds = 0.4;
constexpr auto doubleTapSlopPoints = 5.f;

// Before the native window arrives.
const auto androidInitialContentSize = Point {640.f, 400.f};

// The count the first finger down earns, carried through to its release so the
// mouse events made from it report the same one. Later fingers count 1.
class TapCounter
{
public:
    int began(int id, Point position, double time)
    {
        auto near = std::abs(position.x - lastPosition.x) <= doubleTapSlopPoints
                    && std::abs(position.y - lastPosition.y) <= doubleTapSlopPoints;
        auto soon = time - lastTime <= doubleTapIntervalSeconds;

        count = (near && soon) ? count + 1 : 1;
        lastTime = time;
        lastPosition = position;
        countedId = id;

        return count;
    }

    int countFor(int id) const { return id == countedId ? count : 1; }

private:
    int count = 0;
    int countedId = 0;
    double lastTime = -doubleTapIntervalSeconds * 2.0;
    Point lastPosition;
};

// The activity as the glue reports it, outliving any one Window: the native
// window can arrive before the app has built one.
struct AndroidWindow;

struct AndroidActivity
{
    android_app* app = nullptr;
    AndroidWindow* window = nullptr;

    // Set by APP_CMD_DESTROY: the system took the activity down, which is not
    // the app quitting. It may be about to create another one in this process.
    bool destroyed = false;
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
        hide = lookup.method(controller, "hide", "(I)V");
    }

    jint mask = 0;
    jint ime = 0;
    jmethodID getInsets = nullptr;
    jmethodID show = nullptr;
    jmethodID hide = nullptr;
    jfieldID left = nullptr;
    jfieldID top = nullptr;
    jfieldID right = nullptr;
    jfieldID bottom = nullptr;
};

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
    TapCounter taps;

    // No command comes when the on-screen keyboard shows or hides.
    Threads::Timer insetsTimer {[this] { refreshInsets(); }, insetsRefreshHz};
};

Point androidPointerPosition(const AndroidWindow& window,
                             const AInputEvent* event,
                             size_t index)
{
    return {AMotionEvent_getX(event, index) / window.scale,
            AMotionEvent_getY(event, index) / window.scale};
}

double androidMotionTime(const AInputEvent* event)
{
    return (double) AMotionEvent_getEventTime(event) / nanosecondsPerSecond;
}

// The contact is an ellipse whose axes Android gives as diameters in pixels.
float androidTouchRadius(const AndroidWindow& window,
                         const AInputEvent* event,
                         size_t index)
{
    auto major = AMotionEvent_getTouchMajor(event, index);
    auto minor = AMotionEvent_getTouchMinor(event, index);
    auto diameter = minor > 0.f ? (major + minor) * 0.5f : major;

    return std::max(diameter, 0.f) * 0.5f / window.scale;
}

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
    touch.pos = androidPointerPosition(*window, event, index);
    touch.pressure = AMotionEvent_getPressure(event, index);
    touch.radius = androidTouchRadius(*window, event, index);
    touch.timestamp = androidMotionTime(event);

    auto isFirstFingerDown =
        phase == TouchPhase::Began && AMotionEvent_getPointerCount(event) == 1;

    touch.tapCount = isFirstFingerDown
                         ? window->taps.began(touch.id, touch.pos, touch.timestamp)
                         : window->taps.countFor(touch.id);

    window->contentView->dispatchTouchEvent(touch);
}

// A mouse, a stylus or the emulator's pointer: hovering and the wheel come as
// motion of their own, never as a touch.
bool androidDispatchMouse(AndroidWindow* window,
                          MouseEventType type,
                          const AInputEvent* event)
{
    if (window == nullptr || window->contentView == nullptr)
        return false;

    auto mouse = MouseEvent {};
    mouse.type = type;
    mouse.pos = androidPointerPosition(*window, event, 0);
    mouse.downPos = mouse.pos;
    mouse.modifiers = androidModifiersFromMeta(AMotionEvent_getMetaState(event));
    mouse.timestamp = androidMotionTime(event);

    if (type == MouseEventType::Wheel)
    {
        mouse.delta = {
            -AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HSCROLL, 0),
            AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0)};

        if (mouse.delta.x == 0.f && mouse.delta.y == 0.f)
            return true;
    }

    window->contentView->dispatchMouseEvent(mouse);

    return true;
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
        case AMOTION_EVENT_ACTION_HOVER_ENTER:
            return androidDispatchMouse(window, MouseEventType::Entered, event);

        case AMOTION_EVENT_ACTION_HOVER_MOVE:
            return androidDispatchMouse(window, MouseEventType::Moved, event);

        case AMOTION_EVENT_ACTION_HOVER_EXIT:
            return androidDispatchMouse(window, MouseEventType::Exited, event);

        case AMOTION_EVENT_ACTION_SCROLL:
            return androidDispatchMouse(window, MouseEventType::Wheel, event);

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

std::optional<KeyEvent> androidKeyEvent(const AInputEvent* event)
{
    const auto action = AKeyEvent_getAction(event);

    if (action != AKEY_EVENT_ACTION_DOWN && action != AKEY_EVENT_ACTION_UP)
        return std::nullopt;

    const auto code = AKeyEvent_getKeyCode(event);
    const auto metaState = AKeyEvent_getMetaState(event);

    auto key = KeyEvent {};
    key.keyCode = androidKeyCodeFromNative(code);
    key.characters = androidKeyText(code, metaState);
    key.charactersIgnoringModifiers = androidKeyText(code, 0);

    // Volume, media, power and the like: the system's, and never the app's.
    if (key.keyCode == KeyCode::Unknown && key.characters.empty())
        return std::nullopt;

    key.type =
        action == AKEY_EVENT_ACTION_DOWN ? KeyEventType::Down : KeyEventType::Up;
    key.modifiers = androidModifiersFromMeta(metaState);
    key.isRepeat =
        key.type == KeyEventType::Down && AKeyEvent_getRepeatCount(event) > 0;
    key.timestamp = (double) AKeyEvent_getEventTime(event) / nanosecondsPerSecond;

    return key;
}

// Zero hands the key back to the system, which is how a Back the app leaves
// alone still leaves the app.
int32_t androidHandleKey(AInputEvent* event)
{
    auto key = androidKeyEvent(event);

    if (!key)
        return 0;

    androidKeyboardEvent(*key);

    auto* window = androidActivity().window;

    if (window == nullptr || window->contentView == nullptr)
        return 0;

    return window->contentView->dispatchKeyEvent(*key) ? 1 : 0;
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
            androidKeyboardReset();

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
            activity.destroyed = true;
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
void linuxViewFocused(bool wantsTextInput)
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
        env->CallVoidMethod(
            controller, wantsTextInput ? java->show : java->hide, java->ime);
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

// Called by android_main (AndroidMain-Android.c) around the app's main(), once
// per activity: a recreated activity runs both again on a new thread in the
// same process, so the state they set is set afresh each time.
extern "C" void eacpAndroidStart(android_app* app)
{
    using namespace eacp::Graphics;

    androidActivity() = AndroidActivity {app};

    if (app->activity != nullptr && app->activity->internalDataPath != nullptr)
        eacp::setAndroidDataDirectory(app->activity->internalDataPath);

    eacp::Jni::setJavaVM(app->activity->vm);
    eacp::Jni::setContext(eacp::Jni::currentEnv(), app->activity->clazz);

    importAndroidEnvironment(app->activity);

    app->onAppCmd = androidHandleCommand;
    app->onInputEvent = androidHandleInput;

    eacp::Threads::setLooperEventHandler(androidHandleLooperEvent);
}

// Nonzero when the system destroyed the activity: the process stays, since
// Android may be recreating it. Zero when main() returned on its own, which
// finishes the activity the app left behind.
extern "C" int eacpAndroidFinish(android_app* app)
{
    auto& activity = eacp::Graphics::androidActivity();
    const auto destroyed = activity.destroyed;

    if (destroyed)
        eacp::LOG(
            "Android: the activity was destroyed; the process stays for a recreate");
    else
    {
        eacp::LOG(
            "Android: main() returned; finishing the activity and the process");
        ANativeActivity_finish(app->activity);
    }

    activity = {};

    return destroyed ? 1 : 0;
}
