#include "FileDrag-Windows.h"

#include <utility>

namespace eacp::Graphics
{

WebView::FileDragPoint
    toFileDragPoint(POINT cursorInClient, const RECT& client, float dpiScale)
{
    WebView::FileDragPoint point;
    point.inside = PtInRect(&client, cursorInClient) != FALSE;

    auto scale = dpiScale > 0.f ? dpiScale : 1.f;
    point.x = cursorInClient.x / scale;
    point.y = cursorInClient.y / scale;
    return point;
}

WebView::FileDragPoint fileDragPointFromCursor(HWND hostHwnd, float dpiScale)
{
    POINT cursor {};
    if (!GetCursorPos(&cursor))
        return {};

    RECT client {};
    GetClientRect(hostHwnd, &client);
    ScreenToClient(hostHwnd, &cursor);
    return toFileDragPoint(cursor, client, dpiScale);
}

FileDragSource::FileDragSource(std::function<WebView::FileDragPoint()> readPoint,
                               std::function<void(WebView::FileDragPoint)> onMoved)
    : readPoint(std::move(readPoint))
    , onMoved(std::move(onMoved))
{
}

HRESULT STDMETHODCALLTYPE FileDragSource::QueryInterface(REFIID riid, void** object)
{
    if (riid == IID_IUnknown || riid == IID_IDropSource)
    {
        *object = static_cast<IDropSource*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE FileDragSource::AddRef()
{
    return ++refCount;
}

ULONG STDMETHODCALLTYPE FileDragSource::Release()
{
    auto remaining = --refCount;
    if (remaining == 0)
        delete this;
    return remaining;
}

HRESULT STDMETHODCALLTYPE FileDragSource::QueryContinueDrag(BOOL escapePressed,
                                                            DWORD keyState)
{
    // Escape or the right button aborts; the left button coming up is the
    // drop. Everything else is the drag in flight — report where it is.
    if (escapePressed || (keyState & MK_RBUTTON) != 0)
        return DRAGDROP_S_CANCEL;
    if ((keyState & MK_LBUTTON) == 0)
        return DRAGDROP_S_DROP;

    onMoved(readPoint());
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FileDragSource::GiveFeedback(DWORD)
{
    // The drop target (see FileDragTarget) reports the effect, so the shell
    // picks the right cursor from it. Over our own window that is a copy
    // cursor; over Explorer / another app, whatever they return.
    return DRAGDROP_S_USEDEFAULTCURSORS;
}

HRESULT STDMETHODCALLTYPE FileDragTarget::QueryInterface(REFIID riid, void** object)
{
    if (riid == IID_IUnknown || riid == IID_IDropTarget)
    {
        *object = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE FileDragTarget::AddRef()
{
    return ++refCount;
}

ULONG STDMETHODCALLTYPE FileDragTarget::Release()
{
    auto remaining = --refCount;
    if (remaining == 0)
        delete this;
    return remaining;
}

HRESULT STDMETHODCALLTYPE FileDragTarget::DragEnter(IDataObject*,
                                                    DWORD,
                                                    POINTL,
                                                    DWORD* effect)
{
    *effect = DROPEFFECT_COPY;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FileDragTarget::DragOver(DWORD, POINTL, DWORD* effect)
{
    *effect = DROPEFFECT_COPY;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FileDragTarget::DragLeave()
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FileDragTarget::Drop(IDataObject*,
                                               DWORD,
                                               POINTL,
                                               DWORD* effect)
{
    *effect = DROPEFFECT_COPY;
    return S_OK;
}

} // namespace eacp::Graphics
