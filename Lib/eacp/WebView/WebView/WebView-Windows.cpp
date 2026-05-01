#include "WebView.h"
#include <eacp/Graphics/Helpers/StringUtils-Windows.h>

#include <eacp/Core/Utils/Windows.h>

#include <algorithm>
#include <unordered_map>
#include <queue>
#include <functional>
#include <vector>

#include <objbase.h>

#include <wrl.h>
#include <WebView2.h>

#include <eacp/Core/Threads/EventLoop.h>

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

HWND findHostHwndForView(View* view);

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

namespace
{
constexpr double minZoomLevel = 0.25;
constexpr double maxZoomLevel = 5.0;
constexpr double zoomStep = 1.1;

std::vector<WebView*>& registeredWebViews()
{
    static auto views = std::vector<WebView*>();
    return views;
}

void registerWebView(WebView* view)
{
    registeredWebViews().push_back(view);
}

void unregisterWebView(WebView* view)
{
    auto& list = registeredWebViews();
    list.erase(std::remove(list.begin(), list.end(), view), list.end());
}
} // namespace

struct WebView::Native
{
    Native(WebView& ownerToUse, WebView::Options optionsToUse)
        : owner(ownerToUse)
        , options(std::move(optionsToUse))
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

        auto parentHwnd = findHostHwndForView(&owner);
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

                                applySettings();
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

    void applySettings()
    {
        if (!webView)
            return;

        ComPtr<ICoreWebView2Settings> settings;
        webView->get_Settings(&settings);

        if (settings)
        {
            settings->put_AreDevToolsEnabled(options.debugConsole ? TRUE : FALSE);
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
                    CoTaskMemString uri;
                    args->get_Uri(&uri);
                    auto url = uri.toString();
                    Threads::callAsync([cb = owner.onNavigationStarted, url]()
                                       { cb(url); });
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

                    if (success)
                    {
                        CoTaskMemString uri;
                        webView->get_Source(&uri);
                        auto url = uri.toString();
                        Threads::callAsync([cb = owner.onNavigationFinished, url]()
                                           { cb(url); });
                    }
                    else
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
                    CoTaskMemString title;
                    webView->get_DocumentTitle(&title);
                    auto titleStr = title.toString();
                    Threads::callAsync([cb = owner.onTitleChanged, titleStr]()
                                       { cb(titleStr); });
                    return S_OK;
                })
                .Get(),
            &titleChangedToken);

        webView->add_WebMessageReceived(
            Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this](ICoreWebView2*,
                       ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
                {
                    CoTaskMemString messageRaw;
                    args->get_WebMessageAsJson(&messageRaw);
                    if (!messageRaw)
                        return S_OK;

                    auto json = messageRaw.toString();
                    auto name = extractJsonStringField(json, "name");
                    auto body = extractJsonStringField(json, "body");

                    auto it = messageHandlers.find(name);
                    if (it == messageHandlers.end())
                        return S_OK;

                    auto handler = it->second;
                    Threads::callAsync([handler, body] { handler(body); });
                    return S_OK;
                })
                .Get(),
            &webMessageToken);
    }

    static std::string extractJsonStringField(const std::string& json,
                                              const std::string& key)
    {
        auto needle = "\"" + key + "\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos)
            return {};

        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos)
            return {};

        auto start = json.find('"', pos);
        if (start == std::string::npos)
            return {};
        ++start;

        auto out = std::string();
        for (auto i = start; i < json.size(); ++i)
        {
            auto c = json[i];
            if (c == '\\' && i + 1 < json.size())
            {
                out.push_back(json[i + 1]);
                ++i;
                continue;
            }
            if (c == '"')
                return out;
            out.push_back(c);
        }
        return {};
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
    WebView::Options options;
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
    EventRegistrationToken webMessageToken {};
};

void WebView::initNative(Options options)
{
    impl = std::make_shared<Native>(*this, std::move(options));
    registerWebView(this);
}

WebView::~WebView()
{
    unregisterWebView(this);
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
    }
}

void WebView::removeScriptMessageHandler(const std::string& name)
{
    impl->messageHandlers.erase(name);
}

void WebView::addUserScript(const std::string& source, bool atDocumentStart)
{
    impl->ensureInitialized();
    impl->queueOperation(
        [this, source, atDocumentStart]()
        {
            if (!impl->webView)
                return;

            if (atDocumentStart)
            {
                auto wide = toWideString(source);
                impl->webView->AddScriptToExecuteOnDocumentCreated(wide.c_str(),
                                                                   nullptr);
            }
            else
            {
                evaluateJavaScript(source);
            }
        });
}

void WebView::resized()
{
    View::resized();
    impl->ensureInitialized();
    impl->updateBounds();
}

void WebView::zoomIn()
{
    setZoom(getZoom() * zoomStep);
}

void WebView::zoomOut()
{
    setZoom(getZoom() / zoomStep);
}

void WebView::resetZoom()
{
    setZoom(1.0);
}

void WebView::setZoom(double level)
{
    auto clamped = std::clamp(level, minZoomLevel, maxZoomLevel);
    if (impl->controller)
        impl->controller->put_ZoomFactor(clamped);
}

double WebView::getZoom() const
{
    if (!impl->controller)
        return 1.0;

    double factor = 1.0;
    impl->controller->get_ZoomFactor(&factor);
    return factor;
}

WebView* WebView::focused()
{
    auto foreground = GetForegroundWindow();
    if (!foreground)
        return nullptr;

    for (auto* view: registeredWebViews())
    {
        if (!view->impl || !view->impl->childHwnd)
            continue;

        auto top = GetAncestor(view->impl->childHwnd, GA_ROOT);
        if (top == foreground)
            return view;
    }

    return nullptr;
}

} // namespace eacp::Graphics
