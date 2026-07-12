# Windows: Windows.UI.Composition → DirectComposition port

**Status: RENDERING. The blank-window bug is fixed (inverted AddVisual z-order,
see below). GUI.exe renders the full demo scene, verified against a DPI-aware
screen capture.**

---

## Why we did this

The Windows graphics backend was built on Windows.UI.Composition (WinRT). Three
problems, in order of how much they actually mattered:

1. **The DispatcherQueue subsystem existed solely to satisfy WinRT.** A WinRT
   `Compositor` refuses to be created on a thread without a `DispatcherQueue`.
   That single requirement is why `ThreadUtils-Windows` stood one up, why
   `Timer-Windows` used a `DispatcherQueueTimer`, and why `shutdownMainThread()`
   existed at all — its own comment records that releasing those WinRT smart
   pointers at static-destruction time, after the COM apartment was gone, crashed
   the process with `STATUS_ACCESS_VIOLATION` on exit. DirectComposition has no
   DispatcherQueue requirement, so the whole subsystem and its crash hazard go
   away.

2. **Compile time.** The composition headers are the single most expensive
   include in the codebase. Measured with `-ftime-trace` on a library-only,
   unity-off build: `Layer-Windows.cpp` took 10.51s, of which **8.94s** was the
   `Composition-Windows.h` chain, and **6.76s** of *that* was the SDK ABI header
   `windows.ui.composition.interop.h` — which transitively drags in
   `Windows.System`, `Windows.ApplicationModel`, `Windows.Media.Capture` and the
   rest of the WinRT ABI closure. Ten TUs paid this; they were ~32% of all
   library compile time, against a median TU of 1.18s.

3. **We use almost none of WinRT Composition.** The entire API surface in use was
   `Compositor`, `ContainerVisual`, `SpriteVisual`, `CompositionGraphicsDevice`,
   `CompositionDrawingSurface`, `CompositionSurfaceBrush`, `CompositionStretch`,
   `DesktopWindowTarget`. No animations, no effect brushes, no expression or
   implicit animations, no backdrop. That is exactly DirectComposition's feature
   set — WinRT Composition is a projection over the same compositor engine.

DComp is also Win8+ (vs Win10 1803+ for `DesktopWindowTarget`), so this *lowers*
the version floor. It's what Chromium and Firefox use on Windows.

## The bug that kept it blank, and the fix

`GUI.exe` showed a "blank" window and `TaskBoard.exe` rendered partially. Every
API call succeeded — target, `SetRoot`, `AddVisual`, `CreateSurface`,
`BeginDraw`/`EndDraw`, `Commit` all returned S_OK. The window was in fact not
blank: the uniform dark content measured RGB(26,26,26) — exactly the demo's
first-added full-window `FilledRect {0.1f, 0.1f, 0.1f}`, rendering **on top of
everything else**.

**Root cause: `AddVisual(child, TRUE, nullptr)` inserts at the BOTTOM of the
z-order, not the top.** With a null `referenceVisual`, the `insertAbove` flag
reads inverted: TRUE prepends to the child list (earlier siblings render above
later ones), FALSE appends. The port had mapped WinRT `InsertAtTop` →
`AddVisual(..., TRUE, nullptr)` at every call site — exactly backwards — so
each window painted only whichever child was attached first (GUI: the dark
background rect; TaskBoard: its first-attached elements).

The fix: named wrappers `insertVisualAtTop()` / `insertVisualAtBottom()` in
`DComp-Windows.h` encode the correct mapping, and every call site
(`NativeLayer-Windows.h`, `View-Windows.cpp` ×2, `CompositionHostWindow`,
`GPUView-Windows.cpp` ×2, `WebView-Windows.cpp`) uses them instead of a bare
boolean.

Empirically verified in both directions: with the inverted mapping, debug
squares added to the root *before* the content view rendered on top of it;
after the fix the same squares were covered by the content view, and the full
demo scene (layers, gradients, text, animation) rendered correctly.

## What was validated before writing any code

Two throwaway spikes (see `git log` for this branch; the sources are gone but the
findings hold):

- **The API mapping holds.** DComp device created directly from our existing
  `ID2D1Device`; `CreateTargetForHwnd`; visual tree via `AddVisual`;
  `IDCompositionSurface::BeginDraw` returns an `ID2D1DeviceContext` with the same
  call shape and `offset` POINT as `ICompositionDrawingSurfaceInterop::BeginDraw`;
  `SetContent`; `IDCompositionVisual3::SetOpacity`; `Commit()`. All succeeded and
  **rendered on screen**, with no DispatcherQueue anywhere.

- **The DPI / transform model.** DComp visuals have **no `Size` property and no
  stretch** — `SetContent` draws the surface 1:1 in the visual's local space,
  whereas WinRT's `SpriteVisual.Size()` + `CompositionSurfaceBrush` stretched the
  surface to fit. Since the root visual is scaled by `dpiScale`, a
  DPI-sized surface would be scaled a second time. The spike confirmed the fix:
  give each **content-bearing leaf visual** a `SetTransform(Scale(1/dpiScale))`,
  and keep offsets in logical points — **DComp applies offsets in parent space**,
  so they are not affected by the visual's own transform. Verified: two 100×100
  logical squares at logical (50,50) and (200,50) under a root scaled by 2 landed
  at physical (100,100) and (400,100), pixel-crisp.

  Container visuals (View's) get **no** counter-scale; only leaves with content
  (layer visuals, View's paint visual, GPUView's swapchain visual).

  (The spikes never caught the z-order inversion because each attached a single
  child per parent — z-order needs two siblings to matter.)

## The one real gap vs WinRT: device loss

`ICompositionGraphicsDeviceInterop::SetRenderingDevice()` let the old backend
hot-swap the D2D device after a loss. Visuals and surfaces **survived** — they
merely lost their pixels — and `redrawAllCompositionHosts()` repainted them.

**DComp has no equivalent.** The rendering device is bound at
`DCompositionCreateDevice2()` and cannot be replaced. A lost device therefore
kills the target, every visual and every surface.

The design here: a **generation counter** (`getCompositionGeneration()`).
`DCompCompositor::recreateRenderingDevice()` rebuilds D3D/D2D/DComp and bumps it.
Every holder (`NativeLayerBase`, `View::Native`, `CompositionHostWindow`,
`GPUView::Native`) stamps itself with the generation it was built under and
rebuilds lazily on a mismatch. `rebuildAllCompositionHosts()` re-runs
`initializeComposition()` for each host and re-attaches its content view, then
falls through to the old redraw path.

**This path has never been exercised.** Device loss is hard to force (try
`IDXGIDevice3::Trim` + a TDR, or a driver restart). Assume it is buggy.

## What changed

| File | Change |
|---|---|
| `Graphics/DComp-Windows.h` | **new** — replaces `Composition-Windows.h` + `CompositionInterop-Windows.h`. Device accessors, `commitComposition()`, generation counter, `insertVisualAtTop/Bottom()`. |
| `Graphics/Graphics/D2DFactory-Windows.cpp` | `WinRTCompositor` → `DCompCompositor`. `DCompositionCreateDevice2(d2dDevice)`. No more `CompositionGraphicsDevice` / `ICompositorInterop`. |
| `Graphics/Layers/NativeLayer-Windows.h` | `SpriteVisual`→`IDCompositionVisual2`, `CompositionDrawingSurface`→`IDCompositionSurface`, brush dropped (`SetContent`), counter-scale, generation stamp. |
| `Graphics/Layers/{Shape,Text}Layer-Windows.cpp` | interop QI → direct `surface->BeginDraw/EndDraw`. |
| `Graphics/Window/CompositionHostWindow-Windows.{h,cpp}` | `DesktopWindowTarget`→`IDCompositionTarget`, root `SetTransform`, host registry + `rebuildAllCompositionHosts()`, `commitComposition()` at end of the render pass. |
| `Graphics/View/View-Windows.cpp` | container + paint visuals, DComp paint surface. |
| `GPU/View/GPUView-Windows.cpp` | swapchain via `SetContent(swapChain)` — no interop surface, no `CompositionStretch::Fill`. |
| `WebView/WebView/WebView-Windows.cpp` | `put_RootVisualTarget` takes the `IDCompositionVisual2` directly. |
| `Core/Threads/ThreadUtils-Windows.{h,cpp}` | **DispatcherQueue deleted.** `isMainThread()` now a thread-id compare. `shutdownMainThread()` is a no-op. |
| `Core/Threads/Timer-Windows.cpp` | `DispatcherQueueTimer` → `SetTimer`/`WM_TIMER`. |
| `Core/Threads/EventLoop-Windows.cpp` | `winrt::init_apartment` → `CoInitializeEx(STA)`. |
| `Graphics/Helpers/StringUtils-Windows.h` | `winrt::to_hstring`/`to_string` → `MultiByteToWideChar`/`WideCharToMultiByte` (drops `winrt/base.h` from 9 TUs). |

**Deliberately NOT changed:** the D3D12 stack (`D3D12Context`, `D3D12Types`,
`Buffer`, `Pipeline`, `Shader`) still uses `winrt::com_ptr` as a generic COM
smart pointer. That's `winrt/base.h` (~0.8s/TU), not the expensive composition
headers (~6.5s/TU). Converting it to `Microsoft::WRL::ComPtr` is mechanical churn
for little gain. `Camera-Windows.cpp` keeps cppwinrt too (it uses
`Windows.Media.Capture`; the alternative is Media Foundation, which is worse).
So cppwinrt does **not** leave the build entirely — it's confined to cheap TUs.

## The finding that matters beyond this port

**The test suite passed 386/387 against a backend that drew (effectively)
nothing.** Every graphics, GPU and WebView test went green while every window
was covered by its own background. They do not verify pixels. A single
golden-image or "is the surface non-empty" assertion would have caught this
instantly — and would have localized it to z-order in minutes. Worth closing.
