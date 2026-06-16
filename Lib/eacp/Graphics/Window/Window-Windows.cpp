#include <eacp/Core/Utils/WinInclude.h>

#include "Window.h"
#include "CompositionHostWindow-Windows.h"
#include "../Helpers/StringUtils-Windows.h"
#include "../Helpers/DarkMode-Windows.h"
#include "../Helpers/SystemAppearance.h"
#include <eacp/Core/App/AppEnvironment.h>

// DwmSetWindowAttribute, used for Win11 rounded corners.
#include <dwmapi.h>
#pragma comment(lib, "Dwmapi.lib")

namespace eacp::Graphics
{

static const wchar_t* WINDOW_CLASS_NAME = L"EACPWindowClass";
static bool windowClassRegistered = false;

namespace
{
struct NonClientInsets
{
    int width;
    int height;
};

// The border + title-bar thickness in physical pixels for this window's style,
// used to convert between window-frame and content sizes for the resize and
// minimum-size parity handlers.
NonClientInsets nonClientInsets(HWND hwnd)
{
    auto dpi = GetDpiForWindow(hwnd);
    auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));

    RECT rect = {0, 0, 0, 0};
    AdjustWindowRectExForDpi(&rect, style, FALSE, exStyle, dpi);
    return {rect.right - rect.left, rect.bottom - rect.top};
}
} // namespace

struct Window::Native
{
    Native(const WindowOptions& options)
        : quitCallback(options.effectiveOnQuit())
        , onResize(options.onResize)
        , onWillResize(options.onWillResize)
        , minWidth(options.minWidth)
        , minHeight(options.minHeight)
    {
        // Process-wide DPI awareness (per-monitor v2) is established by
        // initLoopThread() before any app code runs.
        registerWindowClass();
        createWindow(options);
        host.initializeComposition(true);
        host.onContentResized = onResize;
    }

    ~Native() { host.teardown(); }

    static void registerWindowClass()
    {
        if (windowClassRegistered)
            return;

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = windowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = WINDOW_CLASS_NAME;

        RegisterClassExW(&wc);
        windowClassRegistered = true;
    }

    void createWindow(const WindowOptions& options)
    {
        DWORD style = WS_OVERLAPPEDWINDOW;

        if (options.flags.contains(WindowFlags::Borderless))
        {
            style = WS_POPUP;
            framelessRounded = options.cornerRadius.has_value();
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

        // DWM only rounds windows that carry a frame style — a bare
        // WS_POPUP is silently left square even with DWMWCP_ROUND. Keep
        // WS_THICKFRAME so rounding (and the system shadow) apply; the
        // visible frame is removed again in WM_NCCALCSIZE, after the rect
        // above was computed without it so the client size stays exact.
        if (framelessRounded)
            style |= WS_THICKFRAME;

        DWORD exStyle = options.alwaysOnTop ? WS_EX_TOPMOST : 0;
        showWithoutActivating = options.showInactive;

        auto x = CW_USEDEFAULT;
        auto y = CW_USEDEFAULT;
        if (options.initialPosition)
        {
            x = static_cast<int>(options.initialPosition->x * dpiScale);
            y = static_cast<int>(options.initialPosition->y * dpiScale);
        }

        host.hwnd = CreateWindowExW(exStyle,
                                    WINDOW_CLASS_NAME,
                                    wideTitle.c_str(),
                                    style,
                                    x,
                                    y,
                                    rect.right - rect.left,
                                    rect.bottom - rect.top,
                                    nullptr,
                                    nullptr,
                                    GetModuleHandleW(nullptr),
                                    this);

        if (host.hwnd && options.cornerRadius)
            applyRoundedCorners();

        // Match the title bar to the system theme and opt the process into
        // dark menus so any popup the app shows follows suit.
        if (host.hwnd)
        {
            ensureDarkModeAppInitialised();
            applyTitleBarTheme(host.hwnd, isSystemDarkMode());
        }
    }

    // Windows 11+: ask DWM to round the window at the system radius (the
    // requested radius value isn't configurable). Constants declared
    // locally so older SDKs still compile; pre-Win11 DWM ignores the
    // attribute and the window stays square.
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

    // A maximized window overhangs the monitor by its resize frame on every
    // side. With the frame eaten by WM_NCCALCSIZE the client area would
    // inherit that overhang and the content edges would land offscreen, so
    // inset the proposed rect back to the visible area.
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
            // showInactive: reveal without stealing focus (counterpart of
            // macOS orderFront). visibleOnAllWorkspaces has no Windows
            // analogue. The window still activates normally when clicked.
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
        SetForegroundWindow(host.hwnd);
    }

    void setTitle(const std::string& title) const
    {
        auto wideTitle = toWideString(title);
        SetWindowTextW(host.hwnd, wideTitle.c_str());
    }

    void setContentView(View* view)
    {
        host.attachContentView(view);

        // Skip ShowWindow under headless mode (CI without an active session).
        // The HWND + child visual tree are still set up, so WebView2 can
        // initialize and load its page; only the visible surface is suppressed.
        if (host.hwnd && view && !eacp::Apps::getAppEnvironment().headless)
            showWindow();
    }

    // Honour WindowOptions::onWillResize by clamping the dragged window rect
    // (WM_SIZING gives a frame rect; convert to content points, let the callback
    // mutate, convert back, then re-anchor the edge the user is not dragging).
    void dispatchWillResize(RECT* windowRect, WPARAM edge) const
    {
        auto insets = nonClientInsets(host.hwnd);
        auto scale = host.getDpiScale();

        auto clientWidth = (windowRect->right - windowRect->left) - insets.width;
        auto clientHeight = (windowRect->bottom - windowRect->top) - insets.height;

        auto widthInPoints = static_cast<int>(clientWidth / scale);
        auto heightInPoints = static_cast<int>(clientHeight / scale);
        onWillResize(widthInPoints, heightInPoints);

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

    // Honour WindowOptions::minWidth/minHeight (content points) by setting the
    // window's minimum track size in physical pixels for WM_GETMINMAXINFO.
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
    Callback quitCallback = [] {};
    ResizeCallback onResize;
    WillResizeCallback onWillResize;
    int minWidth = 0;
    int minHeight = 0;
    bool showWithoutActivating = false;
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
        // The WS_THICKFRAME a frameless-rounded window keeps for DWM
        // rounding must not produce a visible frame: claim the whole
        // window rect as client area...
        case WM_NCCALCSIZE:
            if (wParam && self->framelessRounded)
            {
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                clampMaximizedClientRect(hwnd, params->rgrc[0]);
                return 0;
            }
            break;

        // ...and for fixed-size windows, keep the edge band the frame would
        // reserve for resize hit-testing behaving as ordinary content. With
        // WindowFlags::Resizable the band stays live, so a frameless window
        // still resizes from its edges (Electron-style).
        case WM_NCHITTEST:
            if (self->framelessRounded && !self->framelessResizable)
                return HTCLIENT;
            break;

        case WM_CLOSE:
            self->quitCallback();
            return 0;

        case WM_DESTROY:
            // Intentionally no PostQuitMessage here. The application's shutdown
            // is driven by Apps::quit() (which is what quitCallback() triggers
            // on the user-initiated WM_CLOSE). Destroying a Window
            // programmatically — e.g. during test teardown — must NOT terminate
            // the event loop, because its pending quit callback would never get
            // a chance to run.
            return 0;

        case WM_GETMINMAXINFO:
            if (self->minWidth > 0 || self->minHeight > 0)
            {
                self->applyMinTrackSize(reinterpret_cast<MINMAXINFO*>(lParam));
                return 0;
            }
            break;

        case WM_SIZING:
            if (self->onWillResize)
            {
                self->dispatchWillResize(reinterpret_cast<RECT*>(lParam), wParam);
                return TRUE;
            }
            break;

        // The user toggled the OS light/dark setting while we are running;
        // recolour the caption to match.
        case WM_SETTINGCHANGE:
            if (isThemeChangeMessage(lParam))
                applyTitleBarTheme(hwnd, isSystemDarkMode());
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

            // The new scale recreates every layer surface (setDpiScale marks
            // them dirty); painting views re-render from the repaint pass,
            // which recreates their surfaces at the new pixel size.
            self->host.rescaleRootVisualToDpi();
            self->host.ensureAllLayersRendered(self->host.contentView);
            repaintViewTree(self->host.contentView);
            return 0;
        }
    }

    if (auto result = self->host.handleCommonMessage(msg, wParam, lParam))
        return *result;

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

Window::Window(const WindowOptions& optionsToUse)
    : options(optionsToUse)
    , impl(optionsToUse)
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
