#pragma once

// Internal to the Windows WebView backend (WebView-Windows.cpp): the drop
// source + target for a native file drag-out. A header rather than part of the
// translation unit only so the logic is unit-testable without a live
// SHDoDragDrop loop — see Tests/WebView/FileDragTests-Windows.cpp.

#include "WebView.h"

#include <eacp/Core/Utils/WinInclude.h>
#include <ole2.h>

#include <atomic>
#include <functional>
#include <utility>

namespace eacp::Graphics
{

// A cursor position in the host window's client space, mapped into the page's
// own client CSS pixels (top-left origin) — the same space the page reads as
// clientX/clientY, so a drop can be hit-tested against the DOM. GetClientRect
// is physical pixels and the WebView lays out at CSS pixels = physical / DPI,
// so divide back down. `inside` is whether the cursor is over the window at
// all — false once it leaves, which is the receiving app's half of the gesture.
inline WebView::FileDragPoint toFileDragPoint(POINT cursorInClient,
                                              const RECT& client,
                                              float dpiScale)
{
    WebView::FileDragPoint point;
    point.inside = PtInRect(&client, cursorInClient) != FALSE;

    auto scale = dpiScale > 0.f ? dpiScale : 1.f;
    point.x = cursorInClient.x / scale;
    point.y = cursorInClient.y / scale;
    return point;
}

// The live cursor in the drag's host window, in page CSS pixels (see above).
inline WebView::FileDragPoint fileDragPointFromCursor(HWND hostHwnd,
                                                      float dpiScale)
{
    POINT cursor {};
    if (!GetCursorPos(&cursor))
        return {};

    RECT client {};
    GetClientRect(hostHwnd, &client);
    ScreenToClient(hostHwnd, &cursor);
    return toFileDragPoint(cursor, client, dpiScale);
}

// A Windows file drag-out is a blocking modal loop (SHDoDragDrop). This drop
// source is how the app hears about it while it runs: QueryContinueDrag fires on
// every mouse move and button/key change inside the loop, so it streams the
// cursor back and decides when the gesture ends. The default source SHDoDragDrop
// uses with a null argument reports nothing — the host app could never follow
// the drag without this. The cursor reader is injected so tests can drive the
// verdict logic without a real cursor (the backend passes
// fileDragPointFromCursor over its host window).
class FileDragSource final: public IDropSource
{
public:
    FileDragSource(std::function<WebView::FileDragPoint()> readPoint,
                   std::function<void(WebView::FileDragPoint)> onMoved)
        : readPoint(std::move(readPoint))
        , onMoved(std::move(onMoved))
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
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

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        auto remaining = --refCount;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed,
                                                DWORD keyState) override
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

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override
    {
        // The drop target (see FileDragTarget) reports the effect, so the shell
        // picks the right cursor from it. Over our own window that is a copy
        // cursor; over Explorer / another app, whatever they return.
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    std::function<WebView::FileDragPoint()> readPoint;
    std::function<void(WebView::FileDragPoint)> onMoved;
    std::atomic<ULONG> refCount {1};
};

// A minimal drop target registered on our own window for the length of a
// drag-out. Its only job is the cursor: without a target the shell paints the
// ⊘ "no drop" badge over our window even where the page will accept the drop,
// because the effect defaults to NONE. Reporting COPY gives the copy cursor
// instead. The drop itself is deliberately a no-op — the app files the drop by
// watching the cursor (onFileDragEnded, like the macOS path), not through OLE —
// so Drop touches neither the data object nor the page; it only echoes the
// effect to avoid a last-instant cursor flicker.
class FileDragTarget final: public IDropTarget
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
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

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        auto remaining = --refCount;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(
        IDataObject*, DWORD, POINTL, DWORD* effect) override
    {
        *effect = DROPEFFECT_COPY;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override
    {
        *effect = DROPEFFECT_COPY;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE Drop(
        IDataObject*, DWORD, POINTL, DWORD* effect) override
    {
        *effect = DROPEFFECT_COPY;
        return S_OK;
    }

private:
    std::atomic<ULONG> refCount {1};
};

} // namespace eacp::Graphics
