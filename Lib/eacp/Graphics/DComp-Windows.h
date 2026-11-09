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
//
// Everything declared here is main-thread only. The compositor is one
// unsynchronised process-wide object built on first use, a recovery replaces
// its devices in place and then calls back into every host, view and listener,
// and its retries run from the event loop. A device pointer read on another
// thread can be released under it. (The D3D11 device is created without
// D3D11_CREATE_DEVICE_SINGLETHREADED for the composition engine's own threads,
// not for callers.)

#include "D2D-Windows.h"

#include <dcomp.h>

#include <cstdint>

struct ID3D11Device;
struct IDXGIDevice;

namespace eacp::Graphics
{

using Microsoft::WRL::ComPtr;

// The factories need no device and are null only if DirectWrite or Direct2D
// itself failed to start. The D3D/D2D devices are null when no rendering device
// could be made.
ID2D1Factory1* getD2DFactory();
IDWriteFactory* getDWriteFactory();
ID3D11Device* getD3DDevice();
IDXGIDevice* getDXGIDevice();
ID2D1Device* getD2DDevice();

// Null, and isCompositorInitialized() false, when DirectComposition is
// unavailable while everything above is up: a session with no desktop
// compositor (a service, a CI runner, a job started over ssh into session 0)
// fails its creation with E_ACCESSDENIED. Nothing reaches the screen then, but
// text and renderToImage still work. EACP_DCOMP_UNAVAILABLE=1 in the
// environment, set before the first graphics call, forces that state on a
// desktop that has a compositor (Tests/Text/NoComposition-WindowsTests.cpp).
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
HRESULT insertVisualAtTop(IDCompositionVisual2* parent, IDCompositionVisual2* child);
HRESULT insertVisualAtBottom(IDCompositionVisual2* parent,
                             IDCompositionVisual2* child);

// Bumped every time the rendering device is replaced. Anything holding a visual,
// surface or target must compare against its own stamp and rebuild on a
// mismatch — see the device-loss note above.
uint64_t getCompositionGeneration();

// Recovers the shared D3D/D2D/DComp devices when `hr` is a device-loss HRESULT
// (DXGI_ERROR_DEVICE_REMOVED/RESET, D2DERR_RECREATE_TARGET). Returns true if a
// replacement rendering device is live; the caller should drop the current
// frame — every host rebuilds and redraws. False for any other HRESULT, and
// when the replacement failed (a GPU still resetting): the devices are then
// null and the recovery is retried from the event loop, backing off from 0.5 s
// to 30 s, until it works. So is a composition device that was up before the
// loss and did not come back, on the rendering device already replaced.
bool handleDeviceLossIfNeeded(HRESULT hr);

// Runs a failed recovery's next attempt now if it is due, for a caller that
// needs a device in a process that may never pump the event loop.
void retryDeviceRecoveryIfDue();

namespace Detail
{
// Test seam: the next `count` passes through `point` fail with `hr`, as a
// device that is still resetting would.
enum class DeviceFault
{
    CreateRenderingDevice,
    CreateCompositionDevice,
    SnapshotCreateDeviceContext,
    SnapshotEndDraw,
    Count
};

void injectDeviceFault(DeviceFault point, HRESULT hr, int count = 1);

// S_OK, or the failure injected at `point`, which it consumes.
HRESULT takeInjectedDeviceFault(DeviceFault point);
} // namespace Detail

} // namespace eacp::Graphics
