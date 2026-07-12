#pragma once

// DirectComposition — the compositor behind Windows.UI.Composition, reached
// directly as classic COM instead of through the WinRT projection. Same engine,
// same visual tree, same D2D-drawn surfaces, but no cppwinrt (a multi-second
// per-TU include) and, crucially, no DispatcherQueue: WinRT's Compositor demands
// one on its thread, which is the only reason eacp ever had that subsystem.
//
// Two behavioural differences the callers must respect:
//
//   * Nothing reaches the screen until commitComposition(). WinRT committed
//     implicitly; DComp batches until the device is told to flush.
//
//   * The rendering device is bound at device-creation time and there is no
//     SetRenderingDevice(). A lost device therefore invalidates every target,
//     visual and surface, not just their pixels. Objects stamp themselves with
//     getCompositionGeneration() and rebuild when it moves.

#include "D2D-Windows.h"

#include <dcomp.h>

#include <cstdint>

struct ID3D11Device;
struct IDXGIDevice;

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

ID2D1Factory1* getD2DFactory();
IDWriteFactory* getDWriteFactory();
ID3D11Device* getD3DDevice();
IDXGIDevice* getDXGIDevice();
ID2D1Device* getD2DDevice();

IDCompositionDesktopDevice* getCompositionDevice();
bool isCompositorInitialized();

// Flushes every pending visual/surface change to the screen. Cheap when nothing
// changed, so callers batch a whole render pass and commit once at the end.
void commitComposition();

// AddVisual's insertAbove flag reads inverted when referenceVisual is null:
// TRUE prepends to the child list (BOTTOM of the z-order — siblings added
// earlier render on top), FALSE appends (TOP). These wrappers carry the WinRT
// Children().InsertAtTop/InsertAtBottom semantics the call sites were written
// against.
inline HRESULT insertVisualAtTop(IDCompositionVisual2* parent,
                                 IDCompositionVisual2* child)
{
    return parent->AddVisual(child, FALSE, nullptr);
}

inline HRESULT insertVisualAtBottom(IDCompositionVisual2* parent,
                                    IDCompositionVisual2* child)
{
    return parent->AddVisual(child, TRUE, nullptr);
}

// Bumped every time the rendering device is replaced. Anything holding a visual,
// surface or target must compare against its own stamp and rebuild on a
// mismatch — see the device-loss note above.
uint64_t getCompositionGeneration();

// Recovers the shared D3D/D2D/DComp devices when `hr` is a device-loss HRESULT
// (DXGI_ERROR_DEVICE_REMOVED/RESET, D2DERR_RECREATE_TARGET). Returns true if
// recovery ran; the caller should drop the current frame — every host rebuilds
// and redraws once the replacement device is live.
bool handleDeviceLossIfNeeded(HRESULT hr);

} // namespace eacp::Graphics
