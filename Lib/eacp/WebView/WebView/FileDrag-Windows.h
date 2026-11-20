#pragma once

// Internal to the Windows WebView backend (WebView-Windows.cpp): the drop
// source for a native file drag-out, and the arithmetic of handing file drags
// on to WebView2. A header rather than part of the translation unit only so the
// logic is unit-testable without a live SHDoDragDrop loop — see
// Tests/WebView/FileDragTests-Windows.cpp.

#include "WebView.h"

#include <eacp/Core/Utils/WinInclude.h>
#include <ole2.h>

#include <atomic>
#include <functional>

namespace eacp::Graphics
{

// A cursor position in the host window's client space, mapped into the page's
// own client CSS pixels (top-left origin) — the same space the page reads as
// clientX/clientY, so a drop can be hit-tested against the DOM. GetClientRect
// is physical pixels and the WebView lays out at CSS pixels = physical / DPI,
// so divide back down. `inside` is whether the cursor is over the window at
// all — false once it leaves, which is the receiving app's half of the gesture.
WebView::FileDragPoint
    toFileDragPoint(POINT cursorInClient, const RECT& client, float dpiScale);

// The live cursor in the drag's host window, in page CSS pixels (see above).
WebView::FileDragPoint fileDragPointFromCursor(HWND hostHwnd, float dpiScale);

// A Windows file drag-out is a blocking modal loop (SHDoDragDrop). This drop
// source is how the app hears about it while it runs: QueryContinueDrag fires on
// every mouse move and button/key change inside the loop, so it streams the
// cursor back and decides when the gesture ends. The default source SHDoDragDrop
// uses with a null argument reports nothing — the host app could never follow
// the drag without this. The cursor reader is injected so tests can drive the
// verdict logic without a real cursor (the backend passes
// fileDragPointFromCursor over its host window).
class FileDragSource final : public IDropSource
{
public:
    FileDragSource(std::function<WebView::FileDragPoint()> readPoint,
                   std::function<void(WebView::FileDragPoint)> onMoved);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed,
                                                DWORD keyState) override;
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override;

private:
    std::function<WebView::FileDragPoint()> readPoint;
    std::function<void(WebView::FileDragPoint)> onMoved;
    std::atomic<ULONG> refCount {1};
};

// A position local to the WebView, in points, as the physical pixels WebView2's
// input calls take: SendMouseInput and the composition controller's drag and
// drop, whose bounds are set in physical pixels (see updateBounds).
POINT toWebViewPixels(Point localPosition, float dpiScale);

// Whether the WebView takes a file drag the host window routes to it, which is
// what decides the copy or the ⊘ cursor. The page says so through the effect
// WebView2 answers with. During the view's own drag-out it is taken anywhere
// over the view: the page follows that drag through onFileDragMoved/Ended and
// files it where it is released, not only where an HTML5 drop would land, and
// on macOS AppKit shows no refusal over our own window either.
bool webViewTakesFileDrag(DWORD pageEffect, bool ownDragOutInProgress);

} // namespace eacp::Graphics
