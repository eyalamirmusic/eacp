#include "WebView-Windows.h"

#include "WebViewMessages.h"
#include <eacp/Core/Utils/Format.h>
#include <eacp/Core/Utils/Strings.h>

#include <algorithm>

#include <shlwapi.h>

namespace eacp::Graphics
{

namespace
{
// A read-only IStream over a bounded byte range of a StreamingResource.
// WebView2 pulls the response body from this stream, so bytes are fetched
// lazily through ResourceReader::read instead of buffering the whole resource
// up front. The stream owns the resource, so its reader (e.g. an open file)
// stays alive exactly as long as WebView2 keeps reading.
class ReaderStream
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IStream>
{
public:
    ReaderStream(StreamingResource resourceToUse,
                 RangeSize startToUse,
                 RangeSize lengthToUse)
        : resource(std::move(resourceToUse))
        , base(startToUse)
        , length(lengthToUse)
    {
    }

    HRESULT STDMETHODCALLTYPE Read(void* out, ULONG count, ULONG* readOut) override
    {
        auto want = std::min(static_cast<RangeSize>(count), length - position);
        auto got = ULONG {0};

        if (want > 0 && resource.read)
            got = static_cast<ULONG>(resource.read(
                base + position,
                ByteSpan {static_cast<std::uint8_t*>(out), (int) want}));

        position += got;

        if (readOut)
            *readOut = got;

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override
    {
        return STG_E_ACCESSDENIED;
    }

    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move,
                                   DWORD origin,
                                   ULARGE_INTEGER* newPosition) override
    {
        auto target = RangeSize {0};

        switch (origin)
        {
            case STREAM_SEEK_SET:
                target = static_cast<RangeSize>(move.QuadPart);
                break;
            case STREAM_SEEK_CUR:
                target = position + static_cast<RangeSize>(move.QuadPart);
                break;
            case STREAM_SEEK_END:
                target = length + static_cast<RangeSize>(move.QuadPart);
                break;
            default:
                return STG_E_INVALIDFUNCTION;
        }

        position = std::min(target, length);

        if (newPosition)
            newPosition->QuadPart = position;

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override
    {
        if (!stat)
            return STG_E_INVALIDPOINTER;

        *stat = STATSTG {};
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = length;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override
    {
        return STG_E_INVALIDFUNCTION;
    }

    HRESULT STDMETHODCALLTYPE CopyTo(IStream*,
                                     ULARGE_INTEGER,
                                     ULARGE_INTEGER*,
                                     ULARGE_INTEGER*) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER,
                                         ULARGE_INTEGER,
                                         DWORD) override
    {
        return STG_E_INVALIDFUNCTION;
    }

    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER,
                                           ULARGE_INTEGER,
                                           DWORD) override
    {
        return STG_E_INVALIDFUNCTION;
    }

    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    StreamingResource resource;
    RangeSize base = 0;
    RangeSize length = 0;
    RangeSize position = 0;
};

std::string readRequestHeader(ICoreWebView2WebResourceRequest* request, LPCWSTR name)
{
    ComPtr<ICoreWebView2HttpRequestHeaders> headers;
    if (FAILED(request->get_Headers(&headers)) || !headers)
        return {};

    BOOL has = FALSE;
    if (FAILED(headers->Contains(name, &has)) || !has)
        return {};

    ComAllocatedWideString value;
    if (FAILED(headers->GetHeader(name, &value)) || !value)
        return {};

    return value.toString();
}

constexpr auto notFoundStatus = 404;
constexpr auto internalErrorStatus = 500;
constexpr auto plainTextContentType = L"Content-Type: text/plain; charset=utf-8";

bool hasCustomSchemes(const WebView::Options& options)
{
    return !options.schemes.empty() || !options.streamingSchemes.empty();
}

template <typename Provider>
const Provider*
    findNonEmptyProvider(const std::unordered_map<std::string, Provider>& providers,
                         const std::string& scheme)
{
    auto found = providers.find(scheme);
    if (found == providers.end() || !found->second)
        return nullptr;

    return &found->second;
}

LPCWSTR statusReason(int statusCode)
{
    switch (statusCode)
    {
        case 200:
            return L"OK";
        case 206:
            return L"Partial Content";
        case 416:
            return L"Range Not Satisfiable";
        default:
            return L"OK";
    }
}
} // namespace

void WebView::Native::registerSchemeHandlers()
{
    if (!webView || !hasCustomSchemes(options))
        return;

    // Stable owners for the per-scheme provider tables. The
    // WebResourceRequested callback captures `this`, looks the request
    // URL's scheme up here, and dispatches to the matching provider --
    // a one-shot ResourceProvider or a chunked StreamingProvider.
    for (auto& [scheme, provider]: options.schemes)
        schemeProviders.emplace(scheme, provider);
    for (auto& [scheme, provider]: options.streamingSchemes)
        streamingProviders.emplace(scheme, provider);

    ensureWebResourceHandler();

    // AddWebResourceRequestedFilter only intercepts URLs that match
    // the filter pattern. Use "scheme://*" so every path under the
    // scheme reaches our handler.
    for (auto& [scheme, _]: options.schemes)
        addWebResourceFilter(scheme + "://*");
    for (auto& [scheme, _]: options.streamingSchemes)
        addWebResourceFilter(scheme + "://*");
}

void WebView::Native::addWebResourceFilter(const std::string& pattern)
{
    auto widePattern = Strings::widen(pattern);
    auto hr = webView->AddWebResourceRequestedFilter(
        widePattern.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);

    if (FAILED(hr))
        LOG("WebView2: AddWebResourceRequestedFilter('" + pattern + "') failed hr="
            + Strings::format("0x{:08X}", static_cast<std::uint32_t>(hr)));
}

// Attach the WebResourceRequested handler once. Both custom-scheme serving
// and loadHTML's base-URL interception route through it, so it must exist
// even when no custom schemes were registered.
void WebView::Native::ensureWebResourceHandler()
{
    if (webResourceToken.value || !webView)
        return;

    webView->add_WebResourceRequested(
        Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this](ICoreWebView2*,
                   ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT
            { return handleWebResourceRequested(args); })
            .Get(),
        &webResourceToken);
}

// Serves an in-memory HTML document for a real (http/https) base URL so the
// page gets that URL's origin -- and, for https, a secure context. macOS's
// loadHTMLString:baseURL: gives the document a trustworthy origin directly;
// WebView2's NavigateToString can't, leaving the page at about:blank where
// secure-context-only APIs (e.g. navigator.mediaDevices) are unavailable.
void WebView::Native::serveInlineHtml(const std::string& baseURL, std::string html)
{
    if (!webView)
        return;

    inlineDocuments[baseURL] = std::move(html);

    ensureWebResourceHandler();

    addWebResourceFilter(baseURL);

    auto wideUrl = Strings::widen(baseURL);
    webView->Navigate(wideUrl.c_str());
}

HRESULT WebView::Native::handleWebResourceRequested(
    ICoreWebView2WebResourceRequestedEventArgs* args)
{
    if (!args || !environment)
        return S_OK;

    ComPtr<ICoreWebView2WebResourceRequest> request;
    if (FAILED(args->get_Request(&request)) || !request)
        return S_OK;

    ComAllocatedWideString uriRaw;
    if (FAILED(request->get_Uri(&uriRaw)) || !uriRaw)
        return S_OK;

    return serveRequestedUrl(args, request.Get(), uriRaw.toString());
}

HRESULT WebView::Native::serveRequestedUrl(
    ICoreWebView2WebResourceRequestedEventArgs* args,
    ICoreWebView2WebResourceRequest* request,
    const std::string& url)
{
    if (auto inlineIt = inlineDocuments.find(url); inlineIt != inlineDocuments.end())
        return serveInlineDocument(args, inlineIt->second);

    auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos)
        return S_OK;

    auto scheme = url.substr(0, schemeEnd);

    if (auto* streamingProvider = findNonEmptyProvider(streamingProviders, scheme))
        return handleStreamingRequest(args, request, url, *streamingProvider);

    if (auto* provider = findNonEmptyProvider(schemeProviders, scheme))
        return handleProviderRequest(args, url, *provider);

    return S_OK;
}

HRESULT WebView::Native::handleProviderRequest(
    ICoreWebView2WebResourceRequestedEventArgs* args,
    const std::string& url,
    const ResourceProvider& provider)
{
    auto response = provider(url);

    if (!response)
        return respondWithError(args, notFoundStatus, L"Not Found");

    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(response->data.data(),
                                    static_cast<UINT>(response->data.size())));
    if (!stream)
        return respondWithError(args, internalErrorStatus, L"Internal Server Error");

    // CORS bypass: WebView2 enforces cross-origin restrictions even
    // for our own scheme handler, so React's fetch() to a sibling
    // URL fails without an explicit allow-origin header.
    auto headers = std::wstring {L"Content-Type: "}
                   + Strings::widen(response->mimeType)
                   + L"\r\nAccess-Control-Allow-Origin: *";

    auto reason = (response->statusCode >= 200 && response->statusCode < 300)
                      ? L"OK"
                      : L"Error";

    respondWith(args, {stream.Get(), response->statusCode, reason, headers});
    return S_OK;
}

HRESULT WebView::Native::respondWithError(
    ICoreWebView2WebResourceRequestedEventArgs* args, int statusCode, LPCWSTR reason)
{
    ComPtr<ICoreWebView2WebResourceResponse> webResponse;
    environment->CreateWebResourceResponse(
        nullptr, statusCode, reason, plainTextContentType, &webResponse);
    args->put_Response(webResponse.Get());
    return S_OK;
}

void WebView::Native::respondWith(ICoreWebView2WebResourceRequestedEventArgs* args,
                                  const WebResourceAnswer& answer)
{
    ComPtr<ICoreWebView2WebResourceResponse> webResponse;

    if (SUCCEEDED(environment->CreateWebResourceResponse(answer.body,
                                                         answer.statusCode,
                                                         answer.reason,
                                                         answer.headers.c_str(),
                                                         &webResponse)))
        args->put_Response(webResponse.Get());
}

HRESULT WebView::Native::serveInlineDocument(
    ICoreWebView2WebResourceRequestedEventArgs* args, const std::string& html)
{
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(html.data()),
                                    static_cast<UINT>(html.size())));

    ComPtr<ICoreWebView2WebResourceResponse> webResponse;
    if (stream
        && SUCCEEDED(environment->CreateWebResourceResponse(
            stream.Get(),
            200,
            L"OK",
            L"Content-Type: text/html; charset=utf-8",
            &webResponse)))
    {
        args->put_Response(webResponse.Get());
    }

    return S_OK;
}

// Serves a custom-scheme request from a StreamingProvider: resolves the
// request's Range header against the resource size and answers 200 / 206 /
// 416 with the matching Accept-Ranges / Content-Range / Content-Length
// headers, streaming the body lazily through a ReaderStream. Mirrors the
// macOS WKURLSchemeTask streaming path.
HRESULT WebView::Native::handleStreamingRequest(
    ICoreWebView2WebResourceRequestedEventArgs* args,
    ICoreWebView2WebResourceRequest* request,
    const std::string& url,
    const StreamingProvider& provider)
{
    auto resource = provider(url);

    if (!resource)
        return respondWithError(args, notFoundStatus, L"Not Found");

    auto plan =
        planStreamingResponse(readRequestHeader(request, L"Range"), *resource);

    auto headers = std::wstring {};
    for (const auto& [name, value]: plan.headers)
    {
        if (!headers.empty())
            headers += L"\r\n";
        headers += Strings::widen(name) + L": " + Strings::widen(value);
    }

    ComPtr<IStream> body;
    if (plan.hasBody)
        body = Microsoft::WRL::Make<ReaderStream>(
            std::move(*resource), plan.served.start, plan.served.length);

    auto reason = statusReason(plan.statusCode);
    respondWith(args, {body.Get(), plan.statusCode, reason, headers});

    return S_OK;
}
} // namespace eacp::Graphics
