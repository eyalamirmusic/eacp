#include "WebView.h"
#include "../../Graphics/Helpers/StringUtils-Windows.h"

#include <unordered_map>
#include <queue>
#include <functional>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <objbase.h>

#include <wrl.h>
#include <WebView2.h>

#include <eacp/Core/Threads/EventLoop.h>

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

using MessageHandlerMap =
    std::unordered_map<std::string, std::function<void(const std::string&)>>;

struct CoTaskMemString
{
    LPWSTR ptr = nullptr;

    ~CoTaskMemString()
    {
        if (ptr)
            CoTaskMemFree(ptr);
    }

    LPWSTR* operator&() { return &ptr; }
    operator LPWSTR() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }

    std::string toString() const
    {
        if (!ptr)
            return "";
        return fromWideString(ptr);
    }
};

HWND findParentHWND(View* view);

struct WebView::Native
{
    Native(WebView& ownerToUse)
        : owner(ownerToUse)
    {
    }

    ~Native()
    {
        if (controller)
        {
            controller->Close();
        }
        if (childHwnd)
        {
            DestroyWindow(childHwnd);
        }
    }

    void ensureInitialized()
    {
        if (initialized || initInProgress)
            return;

        auto parentHwnd = findParentHWND(&owner);
        if (!parentHwnd)
            return;

        initInProgress = true;
        createChildWindow(parentHwnd);
        createWebView2();
    }

    void createChildWindow(HWND parentHwnd)
    {
        static bool classRegistered = false;
        static const wchar_t* CLASS_NAME = L"EACPWebViewHost";

        if (!classRegistered)
        {
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof(WNDCLASSEXW);
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = CLASS_NAME;
            RegisterClassExW(&wc);
            classRegistered = true;
        }

        auto bounds = owner.getLocalBounds();
        childHwnd = CreateWindowExW(0,
                                    CLASS_NAME,
                                    L"",
                                    WS_CHILD | WS_VISIBLE,
                                    static_cast<int>(bounds.x),
                                    static_cast<int>(bounds.y),
                                    static_cast<int>(bounds.w),
                                    static_cast<int>(bounds.h),
                                    parentHwnd,
                                    nullptr,
                                    GetModuleHandleW(nullptr),
                                    nullptr);
    }

    void createWebView2()
    {
        if (!childHwnd)
            return;

        auto hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr,
            nullptr,
            nullptr,
            Microsoft::WRL::Callback<
                ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT
                {
                    if (FAILED(result) || !env)
                    {
                        initInProgress = false;
                        return result;
                    }

                    environment = env;
                    return env->CreateCoreWebView2Controller(
                        childHwnd,
                        Microsoft::WRL::Callback<
                            ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this](HRESULT result,
                                   ICoreWebView2Controller* ctrl) -> HRESULT
                            {
                                if (FAILED(result) || !ctrl)
                                {
                                    initInProgress = false;
                                    return result;
                                }

                                controller = ctrl;
                                controller->get_CoreWebView2(&webView);

                                setupEventHandlers();
                                updateBounds();

                                initialized = true;
                                initInProgress = false;

                                processPendingOperations();

                                return S_OK;
                            })
                            .Get());
                })
                .Get());

        if (FAILED(hr))
        {
            initInProgress = false;
        }
    }

    void setupEventHandlers()
    {
        if (!webView)
            return;

        webView->add_NavigationStarting(
            Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
                [this](ICoreWebView2*,
                       ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT
                {
                    if (owner.onNavigationStarted)
                    {
                        CoTaskMemString uri;
                        args->get_Uri(&uri);
                        auto url = uri.toString();
                        Threads::callAsync([cb = owner.onNavigationStarted, url]()
                                           { cb(url); });
                    }
                    return S_OK;
                })
                .Get(),
            &navigationStartingToken);

        webView->add_NavigationCompleted(
            Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this](ICoreWebView2*,
                       ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT
                {
                    BOOL success = FALSE;
                    args->get_IsSuccess(&success);

                    if (success && owner.onNavigationFinished)
                    {
                        CoTaskMemString uri;
                        webView->get_Source(&uri);
                        auto url = uri.toString();
                        Threads::callAsync([cb = owner.onNavigationFinished, url]()
                                           { cb(url); });
                    }
                    else if (!success && owner.onNavigationFailed)
                    {
                        COREWEBVIEW2_WEB_ERROR_STATUS status;
                        args->get_WebErrorStatus(&status);
                        auto errorStr = "Navigation failed with error: "
                                        + std::to_string(status);
                        Threads::callAsync([cb = owner.onNavigationFailed,
                                            errorStr]() { cb(errorStr); });
                    }
                    return S_OK;
                })
                .Get(),
            &navigationCompletedToken);

        webView->add_DocumentTitleChanged(
            Microsoft::WRL::Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                [this](ICoreWebView2*, IUnknown*) -> HRESULT
                {
                    if (owner.onTitleChanged)
                    {
                        CoTaskMemString title;
                        webView->get_DocumentTitle(&title);
                        auto titleStr = title.toString();
                        Threads::callAsync([cb = owner.onTitleChanged, titleStr]()
                                           { cb(titleStr); });
                    }
                    return S_OK;
                })
                .Get(),
            &titleChangedToken);
    }

    void updateBounds()
    {
        if (!childHwnd)
            return;

        auto globalBounds = calculateGlobalBounds();
        auto dpiScale = getDpiScale();

        auto x = static_cast<int>(globalBounds.x * dpiScale);
        auto y = static_cast<int>(globalBounds.y * dpiScale);
        auto w = static_cast<int>(globalBounds.w * dpiScale);
        auto h = static_cast<int>(globalBounds.h * dpiScale);

        SetWindowPos(childHwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);

        if (controller)
        {
            RECT bounds = {0, 0, w, h};
            controller->put_Bounds(bounds);
        }
    }

    Rect calculateGlobalBounds()
    {
        Rect globalBounds = owner.getLocalBounds();
        View* current = owner.getParent();
        float offsetX = 0;
        float offsetY = 0;

        while (current)
        {
            offsetX += current->getBounds().x;
            offsetY += current->getBounds().y;
            current = current->getParent();
        }

        globalBounds.x = offsetX;
        globalBounds.y = offsetY;

        return globalBounds;
    }

    float getDpiScale()
    {
        if (childHwnd)
        {
            auto dpi = GetDpiForWindow(childHwnd);
            return static_cast<float>(dpi) / 96.f;
        }
        return 1.f;
    }

    void queueOperation(std::function<void()> op)
    {
        if (initialized)
        {
            op();
        }
        else
        {
            pendingOperations.push(std::move(op));
        }
    }

    void processPendingOperations()
    {
        while (!pendingOperations.empty())
        {
            auto op = std::move(pendingOperations.front());
            pendingOperations.pop();
            op();
        }
    }

    WebView& owner;
    HWND childHwnd = nullptr;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webView;
    MessageHandlerMap messageHandlers;

    bool initialized = false;
    bool initInProgress = false;
    std::queue<std::function<void()>> pendingOperations;

    EventRegistrationToken navigationStartingToken {};
    EventRegistrationToken navigationCompletedToken {};
    EventRegistrationToken titleChangedToken {};
};

HWND findParentHWND(View*)
{
    return FindWindowW(L"EACPWindowClass", nullptr);
}

WebView::WebView()
    : impl(*this)
{
}

WebView::~WebView()
{
    if (impl->controller)
    {
        impl->controller->Close();
    }
}

void WebView::loadURL(const std::string& url)
{
    impl->ensureInitialized();
    impl->queueOperation(
        [this, url]()
        {
            if (impl->webView)
            {
                auto wideUrl = toWideString(url);
                impl->webView->Navigate(wideUrl.c_str());
            }
        });
}

void WebView::loadHTML(const std::string& html, const std::string& baseURL)
{
    impl->ensureInitialized();
    impl->queueOperation(
        [this, html, baseURL]()
        {
            if (impl->webView)
            {
                auto wideHtml = toWideString(html);
                impl->webView->NavigateToString(wideHtml.c_str());
            }
        });
}

void WebView::goBack()
{
    if (impl->webView)
    {
        impl->webView->GoBack();
    }
}

void WebView::goForward()
{
    if (impl->webView)
    {
        impl->webView->GoForward();
    }
}

void WebView::reload()
{
    if (impl->webView)
    {
        impl->webView->Reload();
    }
}

void WebView::stopLoading()
{
    if (impl->webView)
    {
        impl->webView->Stop();
    }
}

bool WebView::canGoBack() const
{
    if (!impl->webView)
        return false;

    BOOL canGoBack = FALSE;
    impl->webView->get_CanGoBack(&canGoBack);
    return canGoBack != FALSE;
}

bool WebView::canGoForward() const
{
    if (!impl->webView)
        return false;

    BOOL canGoForward = FALSE;
    impl->webView->get_CanGoForward(&canGoForward);
    return canGoForward != FALSE;
}

bool WebView::isLoading() const
{
    return false;
}

std::string WebView::getURL() const
{
    if (!impl->webView)
        return "";

    CoTaskMemString uri;
    impl->webView->get_Source(&uri);
    return uri.toString();
}

std::string WebView::getTitle() const
{
    if (!impl->webView)
        return "";

    CoTaskMemString title;
    impl->webView->get_DocumentTitle(&title);
    return title.toString();
}

void WebView::evaluateJavaScript(const std::string& script, JSCallback callback)
{
    if (!impl->webView)
    {
        if (callback)
        {
            callback("", "WebView not initialized");
        }
        return;
    }

    auto wideScript = toWideString(script);

    impl->webView->ExecuteScript(
        wideScript.c_str(),
        Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [callback](HRESULT errorCode, LPCWSTR resultJson) -> HRESULT
            {
                if (callback)
                {
                    std::string result;
                    std::string error;

                    if (FAILED(errorCode))
                    {
                        error = "Script execution failed";
                    }
                    else if (resultJson)
                    {
                        result = fromWideString(resultJson);
                    }

                    Threads::callAsync([callback, result, error]()
                                       { callback(result, error); });
                }
                return S_OK;
            })
            .Get());
}

void WebView::addScriptMessageHandler(
    const std::string& name, std::function<void(const std::string& message)> handler)
{
    impl->messageHandlers[name] = std::move(handler);

    if (impl->webView)
    {
        auto script = toWideString("window." + name
                                   + " = { postMessage: function(msg) { "
                                     "window.chrome.webview.postMessage({name: '"
                                   + name + "', body: msg}); } };");
        impl->webView->AddScriptToExecuteOnDocumentCreated(script.c_str(), nullptr);

        impl->webView->add_WebMessageReceived(
            Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this](ICoreWebView2*,
                       ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
                {
                    CoTaskMemString messageRaw;
                    args->get_WebMessageAsJson(&messageRaw);

                    if (messageRaw)
                    {
                        auto message = messageRaw.toString();
                        for (auto& [handlerName, handlerFunc]: impl->messageHandlers)
                        {
                            Threads::callAsync([handlerFunc, message]()
                                               { handlerFunc(message); });
                        }
                    }
                    return S_OK;
                })
                .Get(),
            nullptr);
    }
}

void WebView::removeScriptMessageHandler(const std::string& name)
{
    impl->messageHandlers.erase(name);
}

void WebView::resized()
{
    View::resized();
    impl->ensureInitialized();
    impl->updateBounds();
}

} // namespace eacp::Graphics
