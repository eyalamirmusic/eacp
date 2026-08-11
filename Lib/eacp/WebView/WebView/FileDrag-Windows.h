#pragma once

// Drop source + target for WebView-Windows.cpp's file drag-out. A header only
// so the logic stays testable without a live SHDoDragDrop loop.

#include "WebView.h"

#include <eacp/Core/Utils/WinInclude.h>
#include <ole2.h>

#include <atomic>
#include <functional>
#include <utility>

namespace eacp::Graphics
{

// Maps a client-space cursor into page CSS pixels, which the WebView lays out
// at physical / DPI while GetClientRect reports physical.
inline WebView::FileDragPoint
    toFileDragPoint(POINT cursorInClient, const RECT& client, float dpiScale)
{
    WebView::FileDragPoint point;
    point.inside = PtInRect(&client, cursorInClient) != FALSE;

    auto scale = dpiScale > 0.f ? dpiScale : 1.f;
    point.x = cursorInClient.x / scale;
    point.y = cursorInClient.y / scale;
    return point;
}

inline WebView::FileDragPoint fileDragPointFromCursor(HWND hostHwnd, float dpiScale)
{
    POINT cursor {};
    if (!GetCursorPos(&cursor))
        return {};

    RECT client {};
    GetClientRect(hostHwnd, &client);
    ScreenToClient(hostHwnd, &cursor);
    return toFileDragPoint(cursor, client, dpiScale);
}

// SHDoDragDrop's own default source reports nothing, so this one exists to
// stream the cursor out of the blocking modal loop via QueryContinueDrag.
// readPoint is injected so tests can drive it without a real cursor.
class FileDragSource final : public IDropSource
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
        if (escapePressed || (keyState & MK_RBUTTON) != 0)
            return DRAGDROP_S_CANCEL;
        if ((keyState & MK_LBUTTON) == 0)
            return DRAGDROP_S_DROP;

        onMoved(readPoint());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override
    {
        // The shell derives the cursor from the drop target's effect.
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    std::function<WebView::FileDragPoint()> readPoint;
    std::function<void(WebView::FileDragPoint)> onMoved;
    std::atomic<ULONG> refCount {1};
};

// Exists only to report COPY: with no target registered the shell paints the
// ⊘ badge over our own window. The app files the actual drop by watching the
// cursor (onFileDragEnded), so Drop() deliberately does nothing.
class FileDragTarget final : public IDropTarget
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

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*,
                                        DWORD,
                                        POINTL,
                                        DWORD* effect) override
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

    HRESULT STDMETHODCALLTYPE Drop(IDataObject*,
                                   DWORD,
                                   POINTL,
                                   DWORD* effect) override
    {
        *effect = DROPEFFECT_COPY;
        return S_OK;
    }

private:
    std::atomic<ULONG> refCount {1};
};

} // namespace eacp::Graphics
