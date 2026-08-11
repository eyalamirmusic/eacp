#pragma once

// DirectComposition as classic COM. Nothing reaches the screen until
// commitComposition(), and a lost device invalidates every target, visual and
// surface, so holders rebuild when getCompositionGeneration() moves.

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

// Cheap when nothing changed, so batch a render pass and commit once.
void commitComposition();

// AddVisual's insertAbove flag reads inverted when referenceVisual is null:
// TRUE prepends (bottom of the z-order), FALSE appends (top).
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

// Bumped whenever the rendering device is replaced; holders rebuild on a
// mismatch with their own stamp.
uint64_t getCompositionGeneration();

// Recovers the shared devices when `hr` is a device-loss HRESULT. True means
// recovery ran and the caller should drop the current frame.
bool handleDeviceLossIfNeeded(HRESULT hr);

} // namespace eacp::Graphics
