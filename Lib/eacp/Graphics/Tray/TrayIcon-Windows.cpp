#include <eacp/Core/Utils/WinInclude.h>

#include "TrayIcon.h"
#include "../Helpers/StringUtils-Windows.h"
#include "../Helpers/DarkMode-Windows.h"
#include "../Helpers/ImageConversion-Windows.h"

#include <shellapi.h>

#include <unordered_map>

namespace eacp::Graphics
{

static const std::wstring TRAY_WINDOW_CLASS_NAME_STORAGE =
    eacp::Plugins::getUniqueWindowClassName(L"EACPTrayWindowClass");
static const wchar_t* TRAY_WINDOW_CLASS_NAME =
    TRAY_WINDOW_CLASS_NAME_STORAGE.c_str();
static bool trayWindowClassRegistered = false;

// The notification-area callback message and the icon's per-window id.
constexpr UINT WM_EACP_TRAY = WM_APP + 0x10;
constexpr UINT TRAY_ICON_ID = 1;

struct TrayIcon::Native
{
    Native()
    {
        if (eacp::Apps::getAppEnvironment().headless)
            return;

        // So the popup menu drawn by TrackPopupMenu follows the system theme.
        ensureDarkModeAppInitialised();

        // Set before the window exists: a WM_NCCREATE-time TaskbarCreated check
        // would otherwise read a zero id.
        taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
        registerWindowClass();
        createMessageWindow();
        addIcon();
    }

    ~Native()
    {
        if (messageWindow)
        {
            Shell_NotifyIconW(NIM_DELETE, &nid);
            DestroyWindow(messageWindow);
        }

        if (currentIcon)
            DestroyIcon(currentIcon);

        if (currentMenu)
            DestroyMenu(currentMenu);
    }

    static void registerWindowClass()
    {
        if (trayWindowClassRegistered)
            return;

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = windowProc;
        wc.hInstance = (HINSTANCE) eacp::Plugins::getCurrentModuleHandle();
        wc.lpszClassName = TRAY_WINDOW_CLASS_NAME;

        trayWindowClassRegistered = RegisterClassExW(&wc) != 0;
    }

    // A normal (but never shown) window. Unlike a message-only window it can be
    // made foreground, which TrackPopupMenu needs so the menu dismisses
    // correctly.
    void createMessageWindow()
    {
        messageWindow =
            CreateWindowExW(0,
                            TRAY_WINDOW_CLASS_NAME,
                            L"",
                            WS_POPUP,
                            0,
                            0,
                            0,
                            0,
                            nullptr,
                            nullptr,
                            (HINSTANCE) eacp::Plugins::getCurrentModuleHandle(),
                            this);
    }

    void addIcon()
    {
        if (!messageWindow)
            return;

        nid = {};
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = messageWindow;
        nid.uID = TRAY_ICON_ID;
        nid.uFlags = NIF_MESSAGE;
        nid.uCallbackMessage = WM_EACP_TRAY;

        Shell_NotifyIconW(NIM_ADD, &nid);
    }

    void setIcon(const Image& icon)
    {
        if (!messageWindow)
            return;

        auto newIcon = toHIcon(icon);
        if (!newIcon)
            return;

        if (currentIcon)
            DestroyIcon(currentIcon);
        currentIcon = newIcon;

        nid.uFlags |= NIF_ICON;
        nid.hIcon = currentIcon;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void setTooltip(const std::string& text)
    {
        if (!messageWindow)
            return;

        auto wide = toWideString(text);
        lstrcpynW(nid.szTip, wide.c_str(), ARRAYSIZE(nid.szTip));
        nid.uFlags |= NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void setMenu(const Menu& menu)
    {
        if (currentMenu)
            DestroyMenu(currentMenu);

        commandActions.clear();
        nextCommandId = 1;
        currentMenu = buildMenu(menu);
    }

    void setOnClick(Callback callback) { onClick = std::move(callback); }

    void setOnNotificationClick(Callback callback)
    {
        onNotificationClick = std::move(callback);
    }

    void showNotification(const std::string& title, const std::string& body)
    {
        if (!messageWindow)
            return;

        auto wideTitle = toWideString(title);
        auto wideBody = toWideString(body);
        lstrcpynW(nid.szInfoTitle, wideTitle.c_str(), ARRAYSIZE(nid.szInfoTitle));
        lstrcpynW(nid.szInfo, wideBody.c_str(), ARRAYSIZE(nid.szInfo));

        nid.uFlags |= NIF_INFO;
        nid.dwInfoFlags = NIIF_NONE | NIIF_RESPECT_QUIET_TIME;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    HMENU buildMenu(const Menu& menu)
    {
        auto* hmenu = CreatePopupMenu();

        for (auto& item: menu.items)
            appendItem(hmenu, item);

        return hmenu;
    }

    void appendItem(HMENU hmenu, const MenuItem& item)
    {
        if (item.isSeparator)
        {
            AppendMenuW(hmenu, MF_SEPARATOR, 0, nullptr);
            return;
        }

        auto wide = toWideString(item.title);

        if (item.submenu)
        {
            auto* submenu = buildMenu(*item.submenu);
            AppendMenuW(
                hmenu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), wide.c_str());
            return;
        }

        auto id = nextCommandId++;
        commandActions[id] = item.action;
        AppendMenuW(hmenu, MF_STRING, id, wide.c_str());
    }

    void showMenu()
    {
        if (!currentMenu || !messageWindow)
            return;

        POINT cursor;
        GetCursorPos(&cursor);

        SetForegroundWindow(messageWindow);

        auto cmd = TrackPopupMenu(currentMenu,
                                  TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                  cursor.x,
                                  cursor.y,
                                  0,
                                  messageWindow,
                                  nullptr);

        // Documented workaround so the menu dismisses on the next click.
        PostMessageW(messageWindow, WM_NULL, 0, 0);

        if (cmd != 0)
            dispatchCommand(static_cast<UINT>(cmd));
    }

    void dispatchCommand(UINT id)
    {
        auto it = commandActions.find(id);
        if (it == commandActions.end())
            return;

        auto action = it->second;
        Threads::callAsync(
            [action]
            {
                if (action)
                    action();
            });
    }

    static LRESULT CALLBACK windowProc(HWND hwnd,
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
            self->messageWindow = hwnd;
        }
        else
        {
            self = reinterpret_cast<Native*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (!self)
            return DefWindowProcW(hwnd, msg, wParam, lParam);

        if (msg == self->taskbarCreatedMessage)
        {
            // Explorer restarted and dropped every tray icon; re-add ours.
            Shell_NotifyIconW(NIM_ADD, &self->nid);
            return 0;
        }

        if (msg == WM_EACP_TRAY)
        {
            switch (LOWORD(lParam))
            {
                case WM_LBUTTONUP:
                    if (self->onClick)
                    {
                        auto cb = self->onClick;
                        Threads::callAsync(
                            [cb]
                            {
                                if (cb)
                                    cb();
                            });
                    }
                    break;

                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    self->showMenu();
                    break;

                case NIN_BALLOONUSERCLICK:
                    if (self->onNotificationClick)
                    {
                        auto cb = self->onNotificationClick;
                        Threads::callAsync(
                            [cb]
                            {
                                if (cb)
                                    cb();
                            });
                    }
                    break;
            }
            return 0;
        }

        // Refresh the cached menu theme so an open-after-switch picks up a
        // live light/dark change.
        if (msg == WM_SETTINGCHANGE && isThemeChangeMessage(lParam))
            refreshMenuTheme();

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    HWND messageWindow = nullptr;
    NOTIFYICONDATAW nid = {};
    HICON currentIcon = nullptr;
    HMENU currentMenu = nullptr;
    std::unordered_map<UINT, MenuAction> commandActions;
    UINT nextCommandId = 1;
    UINT taskbarCreatedMessage = 0;
    Callback onClick;
    Callback onNotificationClick;
};

TrayIcon::TrayIcon() = default;
TrayIcon::~TrayIcon() = default;

void TrayIcon::setIcon(const Image& icon)
{
    impl->setIcon(icon);
}

void TrayIcon::setTooltip(const std::string& text)
{
    impl->setTooltip(text);
}

void TrayIcon::setMenu(const Menu& menu)
{
    impl->setMenu(menu);
}

void TrayIcon::setOnClick(Callback callback)
{
    impl->setOnClick(std::move(callback));
}

// Template rendering is a macOS menu-bar concept; nothing to do here.
void TrayIcon::setTemplateRendering(bool) {}

void TrayIcon::showNotification(const std::string& title, const std::string& body)
{
    impl->showNotification(title, body);
}

void TrayIcon::setOnNotificationClick(Callback callback)
{
    impl->setOnNotificationClick(std::move(callback));
}

} // namespace eacp::Graphics
