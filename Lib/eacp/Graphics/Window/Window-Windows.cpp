#include <eacp/Core/Utils/WinInclude.h>

#include "Window.h"
#include "CompositionHostWindow-Windows.h"
#include "../Helpers/StringUtils-Windows.h"
#include "../Helpers/DarkMode-Windows.h"
#include "../Helpers/ImageConversion-Windows.h"
#include "../Helpers/SystemAppearance.h"
#include "../Menu/Win32Menu.h"

#include <dwmapi.h>
#pragma comment(lib, "Dwmapi.lib")

#include <cmath>

namespace eacp::Graphics
{

static const std::wstring WINDOW_CLASS_NAME_STORAGE =
    eacp::Plugins::getUniqueWindowClassName(L"EACPWindowClass");
static const wchar_t* WINDOW_CLASS_NAME = WINDOW_CLASS_NAME_STORAGE.c_str();
static bool windowClassRegistered = false;

namespace
{
struct NonClientInsets
{
    int width;
    int height;
};

// Border + title-bar thickness in physical pixels, for converting between
// window-frame and content sizes.
NonClientInsets nonClientInsets(HWND hwnd)
{
    auto dpi = GetDpiForWindow(hwnd);
    auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));

    RECT rect = {0, 0, 0, 0};
    AdjustWindowRectExForDpi(&rect, style, GetMenu(hwnd) != nullptr, exStyle, dpi);
    return {rect.right - rect.left, rect.bottom - rect.top};
}

} // namespace

struct Window::Native
{
    Native(const WindowOptions& options, WindowEvents& eventsToUse)
        : quitCallback(options.effectiveOnQuit())
        , onResize(options.onResize)
        , onWillResize(options.onWillResize)
        , events(&eventsToUse)
        , minWidth(options.minWidth)
        , minHeight(options.minHeight)
        , aspectRatio(options.aspectRatio)
        , hidesOnClose(options.hidesOnClose)
    {
        // A hosted plugin never ran initLoopThread, so adopt the host UI
        // thread creating the first window as this copy's main thread.
        Threads::attachCurrentThreadAsMain();
        registerWindowClass();
        createWindow(options);
        host.initializeComposition(true);
        host.onContentResized = onResize;
    }

    ~Native()
    {
        // While the handle is still valid, or a later window reusing this HWND
        // address inherits these menu commands.
        detail::removeWin32MenuBar(host.hwnd);

        host.teardown();

        if (applicationIcon)
            DestroyIcon(applicationIcon);

        if (altTabIcon)
            DestroyIcon(altTabIcon);
    }

    static void registerWindowClass()
    {
        if (windowClassRegistered)
            return;

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = windowProc;
        wc.hInstance = (HINSTANCE) eacp::Plugins::getCurrentModuleHandle();
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = WINDOW_CLASS_NAME;

        // Fallback for windows that stamp no WM_SETICON; null keeps the
        // system default.
        wc.hIcon = embeddedApplicationIcon();

        windowClassRegistered = RegisterClassExW(&wc) != 0;
    }

    static RECT activeMonitorWorkArea()
    {
        auto cursor = POINT {};
        GetCursorPos(&cursor);

        auto monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);

        auto info = MONITORINFO {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info))
            return info.rcWork;

        return RECT {
            0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }

    void createWindow(const WindowOptions& options)
    {
        DWORD style = WS_OVERLAPPEDWINDOW;

        host.transparentBackground = options.transparentBackground;

        if (options.flags.contains(WindowFlags::Borderless))
        {
            style = WS_POPUP;

            // The frame DWM rounds is also the one it shadows rectangularly,
            // which would trace a transparent window's see-through surplus.
            framelessRounded =
                options.cornerRadius.has_value() && !options.transparentBackground;
            framelessResizable =
                framelessRounded && options.flags.contains(WindowFlags::Resizable);
        }

        std::wstring wideTitle =
            options.showTitle ? toWideString(options.title) : std::wstring {};

        auto dpi = GetDpiForSystem();
        auto dpiScale = static_cast<float>(dpi) / 96.f;
        auto physicalWidth = static_cast<int>(options.width * dpiScale);
        auto physicalHeight = static_cast<int>(options.height * dpiScale);

        RECT rect = {0, 0, physicalWidth, physicalHeight};
        AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);

        // DWM leaves a bare WS_POPUP square even with DWMWCP_ROUND, so keep
        // WS_THICKFRAME here and drop the visible frame in WM_NCCALCSIZE.
        if (framelessRounded)
            style |= WS_THICKFRAME;

        DWORD exStyle = options.alwaysOnTop ? WS_EX_TOPMOST : 0;
        if (options.ignoresMouseEvents)
            exStyle |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;

        // Without a redirection bitmap the content's own alpha reaches the
        // screen. Cannot be turned on after creation.
        if (options.transparentBackground)
            exStyle |= WS_EX_NOREDIRECTIONBITMAP;

        showWithoutActivating = options.showInactive;
        ignoresMouseEvents = options.ignoresMouseEvents;

        auto windowWidth = rect.right - rect.left;
        auto windowHeight = rect.bottom - rect.top;

        auto x = CW_USEDEFAULT;
        auto y = CW_USEDEFAULT;
        if (options.initialPosition)
        {
            x = static_cast<int>(options.initialPosition->x * dpiScale);
            y = static_cast<int>(options.initialPosition->y * dpiScale);
        }
        else
        {
            auto area = activeMonitorWorkArea();
            x = area.left + ((area.right - area.left) - windowWidth) / 2;
            y = area.top + ((area.bottom - area.top) - windowHeight) / 2;
        }

        host.hwnd =
            CreateWindowExW(exStyle,
                            WINDOW_CLASS_NAME,
                            wideTitle.c_str(),
                            style,
                            x,
                            y,
                            rect.right - rect.left,
                            rect.bottom - rect.top,
                            nullptr,
                            nullptr,
                            (HINSTANCE) eacp::Plugins::getCurrentModuleHandle(),
                            this);

        if (host.hwnd && options.cornerRadius && !options.transparentBackground)
            applyRoundedCorners();

        if (host.hwnd)
        {
            ensureDarkModeAppInitialised();
            applyTitleBarTheme(host.hwnd, isSystemDarkMode());
        }

        if (host.hwnd)
            applyApplicationIcons(options);
    }

    // The ICON resource eacp_set_app_icon compiles in under id 1. Shared, so
    // the result must never be passed to DestroyIcon.
    static HICON embeddedApplicationIcon()
    {
        return LoadIconW((HINSTANCE) eacp::Plugins::getCurrentModuleHandle(),
                         MAKEINTRESOURCEW(1));
    }

    // ICON_SMALL drives the title bar and taskbar, ICON_BIG the Alt-Tab
    // switcher. Unset slots fall back to the class icon.
    void applyApplicationIcons(const WindowOptions& options)
    {
        applicationIcon = toHIcon(options.applicationIcon());
        altTabIcon = toHIcon(options.altTabIcon());

        if (applicationIcon)
            SendMessageW(host.hwnd,
                         WM_SETICON,
                         ICON_SMALL,
                         reinterpret_cast<LPARAM>(applicationIcon));

        if (auto* bigIcon = altTabIcon ? altTabIcon : applicationIcon)
            SendMessageW(
                host.hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));

        if (applicationIcon || embeddedApplicationIcon()
            || eacp::Apps::getAppEnvironment().headless)
            return;

        LOG("This app has no icon: set one with eacp_set_app_icon in "
            "CMake, or provide WindowOptions::applicationIcon for a "
            "dynamic one. The taskbar and Explorer show the generic icon.");
    }

    // Windows 11+ rounds at the system radius, which is not configurable.
    // Constants declared locally so older SDKs still compile; pre-Win11 DWM
    // ignores the attribute.
    void applyRoundedCorners() const
    {
        const DWORD attrWindowCornerPreference =
            33; // DWMWA_WINDOW_CORNER_PREFERENCE
        DWORD preference = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(
            host.hwnd, attrWindowCornerPreference, &preference, sizeof(preference));
    }

    void setVisible(bool visible)
    {
        if (!host.hwnd || eacp::Apps::getAppEnvironment().headless)
            return;

        if (!visible)
        {
            ShowWindow(host.hwnd, SW_HIDE);
            return;
        }

        ShowWindow(host.hwnd, showWithoutActivating ? SW_SHOWNOACTIVATE : SW_SHOW);
    }

    void minimize()
    {
        if (!host.hwnd || eacp::Apps::getAppEnvironment().headless)
            return;

        ShowWindow(host.hwnd, SW_MINIMIZE);
    }

    void toggleMaximize()
    {
        if (!host.hwnd || eacp::Apps::getAppEnvironment().headless)
            return;

        ShowWindow(host.hwnd, IsZoomed(host.hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    }

    // A maximized window overhangs the monitor by its resize frame; with that
    // frame eaten by WM_NCCALCSIZE the content edges would land offscreen.
    static void clampMaximizedClientRect(HWND hwnd, RECT& rect)
    {
        if (!IsZoomed(hwnd))
            return;

        auto dpi = GetDpiForWindow(hwnd);
        auto frame = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi)
                     + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        InflateRect(&rect, -frame, -frame);
    }

    void showWindow() const
    {
        if (host.hwnd)
        {
            ShowWindow(host.hwnd,
                       showWithoutActivating ? SW_SHOWNOACTIVATE : SW_SHOW);
            UpdateWindow(host.hwnd);
        }
    }

    void toFront() const
    {
        if (!host.hwnd || eacp::Apps::getAppEnvironment().headless)
            return;

        ShowWindow(host.hwnd, SW_SHOW);
        forceForeground(host.hwnd);
    }

    static void forceForeground(HWND hwnd)
    {
        auto foreground = GetForegroundWindow();
        auto thisThread = GetCurrentThreadId();
        auto foregroundThread =
            foreground ? GetWindowThreadProcessId(foreground, nullptr) : thisThread;

        auto attached = foregroundThread != thisThread
                        && AttachThreadInput(foregroundThread, thisThread, TRUE);

        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);

        if (attached)
            AttachThreadInput(foregroundThread, thisThread, FALSE);
    }

    void setTitle(const std::string& title) const
    {
        auto wideTitle = toWideString(title);
        SetWindowTextW(host.hwnd, wideTitle.c_str());
    }

    void setContentView(View* view)
    {
        host.attachContentView(view);

        // Headless still builds the HWND and visual tree so WebView2 can load;
        // only the visible surface is suppressed.
        if (host.hwnd && view && !eacp::Apps::getAppEnvironment().headless)
            showWindow();
    }

    // Which side gives way follows the dragged edge, so the window never
    // appears to resist the cursor. Win32 has no setContentAspectRatio.
    void applyAspectRatio(int& widthInPoints, int& heightInPoints, WPARAM edge) const
    {
        if (!aspectRatio || aspectRatio->x <= 0.f || aspectRatio->y <= 0.f)
            return;

        const auto ratio = aspectRatio->x / aspectRatio->y;

        if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM)
            widthInPoints = static_cast<int>(std::lround(heightInPoints * ratio));
        else
            heightInPoints = static_cast<int>(std::lround(widthInPoints / ratio));
    }

    // WM_SIZING gives a frame rect; constrain in content points, convert back,
    // then re-anchor the edge the user is not dragging.
    void dispatchWillResize(RECT* windowRect, WPARAM edge) const
    {
        auto insets = nonClientInsets(host.hwnd);
        auto scale = host.getDpiScale();

        auto clientWidth = (windowRect->right - windowRect->left) - insets.width;
        auto clientHeight = (windowRect->bottom - windowRect->top) - insets.height;

        auto widthInPoints = static_cast<int>(clientWidth / scale);
        auto heightInPoints = static_cast<int>(clientHeight / scale);

        if (onWillResize)
            onWillResize(widthInPoints, heightInPoints);

        // Last, so the locked shape wins over whatever the callback did.
        applyAspectRatio(widthInPoints, heightInPoints, edge);

        auto newWindowWidth = static_cast<int>(widthInPoints * scale) + insets.width;
        auto newWindowHeight =
            static_cast<int>(heightInPoints * scale) + insets.height;

        if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
            windowRect->left = windowRect->right - newWindowWidth;
        else
            windowRect->right = windowRect->left + newWindowWidth;

        if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
            windowRect->top = windowRect->bottom - newWindowHeight;
        else
            windowRect->bottom = windowRect->top + newWindowHeight;
    }

    // Converts WindowOptions::minWidth/minHeight from content points to the
    // physical-pixel track size WM_GETMINMAXINFO wants.
    void applyMinTrackSize(MINMAXINFO* info) const
    {
        auto insets = nonClientInsets(host.hwnd);
        auto scale = host.getDpiScale();

        if (minWidth > 0)
            info->ptMinTrackSize.x =
                static_cast<LONG>(minWidth * scale) + insets.width;
        if (minHeight > 0)
            info->ptMinTrackSize.y =
                static_cast<LONG>(minHeight * scale) + insets.height;
    }

    bool isKeyPressed(uint16_t vk) const { return host.isKeyPressed(vk); }
    bool isShiftPressed() const { return host.isShiftPressed(); }
    bool isControlPressed() const { return host.isControlPressed(); }
    bool isAltPressed() const { return host.isAltPressed(); }
    bool isCommandPressed() const { return host.isCommandPressed(); }
    ModifierKeys getModifiers() const { return host.getModifiers(); }

    static LRESULT CALLBACK windowProc(HWND hwnd,
                                       UINT msg,
                                       WPARAM wParam,
                                       LPARAM lParam);

    CompositionHostWindow host;
    HICON applicationIcon = nullptr;
    HICON altTabIcon = nullptr;
    Callback quitCallback = [] {};
    ResizeCallback onResize;
    WillResizeCallback onWillResize;
    WindowEvents* events = nullptr;
    int minWidth = 0;
    int minHeight = 0;
    std::optional<Point> aspectRatio;
    bool hidesOnClose = false;
    bool showWithoutActivating = false;
    bool ignoresMouseEvents = false;
    bool framelessRounded = false;
    bool framelessResizable = false;
};

LRESULT CALLBACK Window::Native::windowProc(HWND hwnd,
                                            UINT msg,
                                            WPARAM wParam,
                                            LPARAM lParam)
{
    Native* self = nullptr;

    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Native*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->host.hwnd = hwnd;
    }
    else
    {
        self = reinterpret_cast<Native*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self)
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg)
    {
        // Claim the whole window rect as client area, so the WS_THICKFRAME a
        // frameless-rounded window keeps for DWM never draws.
        case WM_NCCALCSIZE:
            if (wParam && self->framelessRounded)
            {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                clampMaximizedClientRect(hwnd, params->rgrc[0]);
                return 0;
            }
            break;

        // For fixed-size windows the frame's resize band behaves as ordinary
        // content; with WindowFlags::Resizable it stays live.
        case WM_NCHITTEST:
            if (self->ignoresMouseEvents)
                return HTTRANSPARENT;

            if (self->framelessRounded && !self->framelessResizable)
                return HTCLIENT;
            break;

        case WM_ACTIVATE:
            if (self->events && self->events->onActivationChanged)
                self->events->onActivationChanged(LOWORD(wParam) != WA_INACTIVE);
            break;

        case WM_CLOSE:
            if (self->hidesOnClose)
            {
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }

            self->quitCallback();
            return 0;

        case WM_DESTROY:
            // No PostQuitMessage: shutdown is driven by Apps::quit, so
            // destroying a Window programmatically must not end the loop.
            return 0;

        case WM_GETMINMAXINFO:
            if (self->minWidth > 0 || self->minHeight > 0)
            {
                self->applyMinTrackSize(reinterpret_cast<MINMAXINFO*>(lParam));
                return 0;
            }
            break;

        case WM_SIZING:
            if (self->onWillResize || self->aspectRatio)
            {
                self->dispatchWillResize(reinterpret_cast<RECT*>(lParam), wParam);
                return TRUE;
            }
            break;

        case WM_SETTINGCHANGE:
            if (isThemeChangeMessage(lParam))
            {
                applyTitleBarTheme(hwnd, isSystemDarkMode());
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            break;

        case WM_DPICHANGED:
        {
            auto* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd,
                         nullptr,
                         suggested->left,
                         suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);

            self->host.rescaleRootVisualToDpi();
            self->host.ensureAllLayersRendered(self->host.contentView);
            repaintViewTree(self->host.contentView);
            return 0;
        }
    }

    // lParam == 0 is the documented test for "came from a menu"; HIWORD alone
    // is not, since BN_CLICKED is also 0 and eacp hosts child windows.
    if (msg == WM_COMMAND && HIWORD(wParam) == 0 && lParam == 0)
        if (detail::handleWin32MenuCommand(hwnd, LOWORD(wParam)))
            return 0;

    // Win32's equivalent of NSMenuValidation, asked just before a popup draws.
    if (msg == WM_INITMENUPOPUP && HIWORD(lParam) == FALSE)
        detail::updateWin32MenuEnabledState(hwnd);

    if (auto result = self->host.handleCommonMessage(msg, wParam, lParam))
        return *result;

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

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

void* Window::getHandle()
{
    return impl->host.hwnd;
}

void* Window::getContentViewHandle()
{
    return impl->host.hwnd;
}

void Window::setContentView(View& view)
{
    contentLink.attach(&view, this);
    impl->setContentView(&view);
}

void Window::toFront()
{
    impl->toFront();
}

void Window::setVisible(bool visible)
{
    impl->setVisible(visible);
}

bool Window::isVisible()
{
    return impl->host.hwnd && IsWindowVisible(impl->host.hwnd);
}

void Window::minimize()
{
    impl->minimize();
}

void Window::toggleMaximize()
{
    impl->toggleMaximize();
}

void Window::setMouseLocked(bool locked)
{
    impl->host.setMouseLocked(locked);
}

bool Window::isMouseLocked() const
{
    return impl->host.isMouseLocked();
}

bool Window::isKeyPressed(uint16_t virtualKeyCode) const
{
    return impl->isKeyPressed(virtualKeyCode);
}

bool Window::isShiftPressed() const
{
    return impl->isShiftPressed();
}

bool Window::isControlPressed() const
{
    return impl->isControlPressed();
}

bool Window::isAltPressed() const
{
    return impl->isAltPressed();
}

bool Window::isCommandPressed() const
{
    return impl->isCommandPressed();
}

ModifierKeys Window::getModifiers() const
{
    return impl->getModifiers();
}

} // namespace eacp::Graphics
