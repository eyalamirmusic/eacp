#pragma once

#include <eacp/Core/Utils/WinInclude.h>

#include "WebView.h"
#include "MessageOrigin.h"
#include "StreamingRange.h"
#include "WebViewDetail.h"
#include <eacp/Graphics/DComp-Windows.h>

#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>

#include <wrl.h>
#include <WebView2.h>

namespace eacp::Graphics
{
using Microsoft::WRL::ComPtr;

HWND findHostHwndForView(View* view);

// Defined in Graphics/Keyboard-Windows.cpp: Windows virtual key -> framework
// KeyCode (KeyCode::Unknown when unmapped). Lets forwarded keys carry the same
// keyCode the host window's own WM_KEYDOWN path would produce.
uint16_t keyCodeFromVirtualKey(int vk);

using MessageHandlerMap =
    std::unordered_map<std::string, std::function<void(const std::string&)>>;

// TODO: move to Core's Windows utilities and replace the hand-written CoTaskMemFree
// calls in App-Windows.cpp, FilePath-Windows.cpp, Dxc-Windows.cpp and
// FileDragOut-Windows.cpp.
struct ComAllocatedWideString
{
    ~ComAllocatedWideString()
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
        return Strings::narrow(ptr);
    }

    LPWSTR ptr = nullptr;
};

// Carries what a window.open popup needs to splice into WebView2's
// NewWindowRequested flow: the opener's environment (the adopted CoreWebView2
// must share it), the opener's options (so the popup serves the same schemes),
// and the callback that hands our CoreWebView2 back once it is live.
struct WebView::PopupInit
{
    ComPtr<ICoreWebView2Environment> environment;
    WebView::Options options;
    std::function<void(ICoreWebView2*)> onReady;
};

struct WebResourceAnswer
{
    IStream* body = nullptr;
    int statusCode = 0;
    LPCWSTR reason = nullptr;
    std::wstring headers;
};

struct WebView::Native
{
    Native(WebView& ownerToUse, WebView::Options optionsToUse);
    ~Native();

    void updateBounds();
    float getDpiScale();
    void notifyParentWindowMoved();
    void setControllerVisible(bool visibleToUse);
    void setViewVisible(bool visibleToUse);
    void applyControllerVisibility();
    void queueOperation(std::function<void()> op);
    void queueDocStartScript(std::wstring script);
    void processPendingOperations();
    void evaluateScript(const std::string& script,
                        const WebView::JSCallback& callback);
    void detachRenderVisual();

    void ensureInitialized();
    void createRenderVisual();
    void createWebView2();
    HRESULT handleEnvironmentCreated(HRESULT result, ICoreWebView2Environment* env);
    HRESULT handleCreateFailure(HRESULT result);
    bool shouldRetryWebView2Create() const;
    HRESULT retryWebView2Create();
    HRESULT createCompositionController();
    HRESULT
    handleCompositionControllerCreated(HRESULT result,
                                       ICoreWebView2CompositionController* ctrl);
    void finishInitialization();
    ComPtr<ICoreWebView2EnvironmentOptions> buildEnvironmentOptions();
    void applySettings();
    void applyUserAgent(const ComPtr<ICoreWebView2Settings>& settings);
    void applyBackground();

    void registerSchemeHandlers();
    void addWebResourceFilter(const std::string& pattern);
    void ensureWebResourceHandler();
    void serveInlineHtml(const std::string& baseURL, std::string html);
    HRESULT
    handleWebResourceRequested(ICoreWebView2WebResourceRequestedEventArgs* args);
    HRESULT
    handleProviderRequest(ICoreWebView2WebResourceRequestedEventArgs* args,
                          const std::string& url,
                          const ResourceProvider& provider);
    HRESULT serveInlineDocument(ICoreWebView2WebResourceRequestedEventArgs* args,
                                const std::string& html);
    HRESULT
    handleStreamingRequest(ICoreWebView2WebResourceRequestedEventArgs* args,
                           ICoreWebView2WebResourceRequest* request,
                           const std::string& url,
                           const StreamingProvider& provider);
    void respondWith(ICoreWebView2WebResourceRequestedEventArgs* args,
                     const WebResourceAnswer& answer);
    HRESULT serveRequestedUrl(ICoreWebView2WebResourceRequestedEventArgs* args,
                              ICoreWebView2WebResourceRequest* request,
                              const std::string& url);
    HRESULT respondWithError(ICoreWebView2WebResourceRequestedEventArgs* args,
                             int statusCode,
                             LPCWSTR reason);

    bool isFromTrustedSender(ICoreWebView2WebMessageReceivedEventArgs* args);
    void setupEventHandlers();
    void removeEventHandlers();
    void addNavigationStartingHandler();
    void addNavigationCompletedHandler();
    void addDocumentTitleChangedHandler();
    void addWebMessageReceivedHandler();
    void addPermissionRequestedHandler();
    void addNewWindowRequestedHandler();
    void addWindowCloseRequestedHandler();
    void handleNavigationCompleted(ICoreWebView2NavigationCompletedEventArgs* args);
    void handleWebMessageReceived(ICoreWebView2WebMessageReceivedEventArgs* args);
    HRESULT
    handleNewWindowRequested(ICoreWebView2NewWindowRequestedEventArgs* args);

    void sendMouse(COREWEBVIEW2_MOUSE_EVENT_KIND kind,
                   const Point& localPos,
                   uint32_t mouseData = 0,
                   COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS virtualKeys =
                       COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE);
    void handleMouseDown(const MouseEvent& event);
    void handleMouseUp(const MouseEvent& event);
    void moveFocusToContent();
    void handleMouseMove(const MouseEvent& event);
    void handleMouseWheel(const MouseEvent& event);
    void handleMouseLeave();
    bool startArmedFileDragIfNeeded();
    void performFileDrag(const Vector<std::string>& paths);
    bool startArmedWindowDragIfNeeded();
    void performWindowDrag();
    void resetDragArming();

    WebView& owner;
    WebView::Options options;
    detail::MessageOriginPolicy originPolicy {options};
    HWND hostHwnd = nullptr;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2CompositionController> compositionController;
    ComPtr<ICoreWebView2> webView;

    // The composition visual the browser renders into. It is a child of the
    // WebView's own View ContainerVisual, so it inherits the View tree's
    // position, opacity and z-order — letting the WebView layer, blend and
    // overlap with GPU and primitive content (impossible with a child HWND).
    Microsoft::WRL::ComPtr<IDCompositionVisual2> webViewVisual;
    MessageHandlerMap messageHandlers;
    std::unordered_map<std::string, ResourceProvider> schemeProviders;
    std::unordered_map<std::string, StreamingProvider> streamingProviders;

    // base URL -> HTML body, for loadHTML() calls that carry a base URL (see
    // serveInlineHtml). Keyed by the exact URL the navigation requests.
    std::unordered_map<std::string, std::string> inlineDocuments;

    bool initialized = false;
    bool initInProgress = false;

    // Whether the host window is showing. See setControllerVisible.
    bool hostVisible = true;

    // Whether this view is shown in its own right. See setViewVisible.
    bool viewVisible = true;

    static constexpr int maxWebView2CreateRetries = 3;
    int webView2CreateRetries = 0;

    // Cleared by the destructor, so a queued create retry can tell whether the
    // Native that scheduled it is still there.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    // Tracks navigation in flight (NavigationStarting -> Completed), so
    // isLoading() can answer like the macOS backend's webView.isLoading.
    bool loading = false;

    // Popup mode (set via PopupInit when adopting a window.open): reuse the
    // opener's environment and, once our CoreWebView2 is live, hand it back to
    // the opener through onCoreWebViewReady so it can call put_NewWindow.
    ComPtr<ICoreWebView2Environment> sharedEnvironment;
    std::function<void(ICoreWebView2*)> onCoreWebViewReady;

    // Set by armFileDrag (the bridge command); consumed by the next
    // mouseDragged, which starts the OS drag for these paths.
    bool dragArmed = false;
    Vector<std::string> armedDragPaths;

    // Set by armWindowDrag (the __eacpWindowDrag bridge command); consumed by
    // the next mouseDragged, which hands the gesture to the OS move loop.
    bool windowDragArmed = false;

    std::queue<std::function<void()>> pendingOperations;
    Vector<std::wstring> pendingDocStartScripts;

    EventRegistrationToken navigationStartingToken {};
    EventRegistrationToken navigationCompletedToken {};
    EventRegistrationToken titleChangedToken {};
    EventRegistrationToken webMessageToken {};
    EventRegistrationToken webResourceToken {};
    EventRegistrationToken permissionRequestedToken {};
    EventRegistrationToken newWindowRequestedToken {};
    EventRegistrationToken windowCloseRequestedToken {};
};

} // namespace eacp::Graphics
