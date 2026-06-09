# Windowing & WebView Platform Cleanup — Analysis & Fix Plan

**Branch:** `windows-windowing-cleanup`
**Author:** code review of `Lib/eacp/Graphics/Window/*` and `Lib/eacp/WebView/WebView/*`
**Status:** analysis complete, no code changes applied yet.

## How to use this document

This is a hand-off plan. The fixes below live almost entirely in Windows-only
translation units (`*-Windows.cpp`), which **do not compile on macOS** — they are
gated into the `elseif(WIN32)` branch of the CMakeLists. They must be built and
tested on Windows:

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF
cmake --build build
```

Work the items in order (A1 → A2 → A3, then B/C/D as desired). A1 and A2 shrink
and de-risk `Window-Windows.cpp` *before* the larger A3 refactor operates on it.
Run `clang-format` on every edited file (project requirement) and run the WebView
tests after any change to the WebView layer.

> ⚠️ **Unity-build hazard (read before A3 / B1).** CI builds with unity on
> (`EACP_CI_BUILD` / `EACP_UNITY_BUILD`), concatenating `Window-Windows.cpp`,
> `EmbeddedView-Windows.cpp`, and `WebView-Windows.cpp` into one TU. They avoid
> symbol clashes today only because they use *different* names (`VK` vs
> `VKEmbedded`, `getXFromLParam` vs `getXFromLParamEmbedded`, separate anonymous
> `registeredWebViews()`). Any code you hoist into a shared location must be
> defined **once** — in a `.cpp`, or `inline` in a header — never re-defined per
> TU. Precedent: `WebView/JsStringLiteral.h` is `inline` in a header specifically
> to dodge an anonymous-namespace ODR clash under unity. Build with
> `-DEACP_CI_BUILD=ON` once at the end to confirm unity still links.

---

## Summary of findings

| # | Finding | Files | Effort | Risk | Build-verifiable on macOS? |
|---|---------|-------|--------|------|----------------------------|
| **A1** | `getWindowDpiScale()` throws a C++ exception on every mouse/paint message | `Window-Windows.cpp` | trivial | low | ❌ Windows-only |
| **A2** | Dead WinRT pointer-input apparatus (~100 lines) | `Window-Windows.cpp` | small | low | ❌ Windows-only |
| **A3** | `Window-Windows.cpp` ↔ `EmbeddedView-Windows.cpp` duplication (~300 lines) | both | large | medium | ❌ Windows-only |
| **B1** | WebView registry duplicated per platform | `WebView.mm`, `WebView-Windows.cpp` | small | low | partial |
| **B2** | Zoom constants + `zoomIn/Out/resetZoom` duplicated verbatim | `WebView.mm`, `WebView-Windows.cpp` | small | low | partial |
| **B3** | Streaming response (status/header) assembly duplicated in structure | `WebView.mm`, `WebView-Windows.cpp` | medium | medium | partial |
| **B4** | Hand-rolled JSON parsing where `Miro::Json` is available | `WebView-Windows.cpp` | medium | medium | ❌ Windows-only |
| **C1** | `NSLog` left in the macOS file-drag hot path | `WebViewPlatform-macOS.mm` | trivial | low | ✅ |
| **C2** | Dead `ownerWindow` member; dead `onResizeRequest` API | `Window-Windows.cpp`, `EmbeddedView.h` | trivial | low | partial |
| **D** | Cross-platform parity gaps (onResize, minSize, mouse-leave on Windows) | windowing | varies | medium | ❌ Windows-only |

---

# Part A — Windows windowing cleanup

## A1. `getWindowDpiScale()` throws on the hot path

**Where:** `Lib/eacp/Graphics/Window/Window-Windows.cpp:249-262`

**Current:**
```cpp
float getWindowDpiScale() const
{
    try
    {
        auto displayInfo = winrt::Windows::Graphics::Display::
            DisplayInformation::GetForCurrentView();
        return displayInfo.LogicalDpi() / 96.f;
    }
    catch (...)
    {
        auto dpi = GetDpiForWindow(hwnd);
        return dpi / 96.f;
    }
}
```

**Problem:** `DisplayInformation::GetForCurrentView()` requires a `CoreWindow` on
the calling thread. This is a Win32 desktop app using `DesktopWindowTarget` — it
has no `CoreWindow` — so the call **throws `winrt::hresult_error` on every
invocation**, and the `catch(...)` fallback runs every time. `getWindowDpiScale()`
is called on the hot path: every `WM_MOUSEMOVE`, `WM_PAINT`, button, and wheel
message (see all the call sites at `:349, :400, :426, :453, :510, :532, :568,
:589, :612, :653`). That's a thrown+caught exception per mouse-move.
`EmbeddedView-Windows.cpp:132` already does the right thing.

**Fix:** mirror `EmbeddedView`'s direct query:
```cpp
float getWindowDpiScale() const
{
    auto dpi = hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
    return dpi / 96.f;
}
```

**Also:** remove `#include <winrt/Windows.Graphics.Display.h>` (`:15`) if nothing
else in the file uses it after this change (grep the file for
`Graphics::Display` / `DisplayInformation` — should be zero hits afterward).

**Optional follow-up (not required):** cache the scale in a member and refresh it
in the `WM_DPICHANGED` handler (`:521`) and on window creation, instead of
recomputing per event. `GetDpiForWindow` is cheap, so this is polish, not a fix.

**Verify:** Windows build; confirm DPI scaling still correct on a 150%/200%
display; no behavioral change expected beyond removing the exception churn.

---

## A2. Delete the dead WinRT pointer-input apparatus

**Where:** `Lib/eacp/Graphics/Window/Window-Windows.cpp` (multiple spots)

**Problem:** `Window` has a complete `CoreIndependentInputSource` pointer-input
path that is **never activated**. `useWinRTPointerInput` is only ever set to
`false` (`:228`), and `inputSource` is only ever assigned `nullptr` (`:117`) —
it's never bound to a real input source. So the three `onPointer*` handlers, the
event tokens, and the five `if (self->useWinRTPointerInput) break;` guards in
`windowProc` are all unreachable. All mouse input flows through the `WM_*`
messages.

### A2a — The local `DispatcherQueueController` is also redundant (proof)

`initializePointerInput()` (`:209`) does only two things: create a local
`DispatcherQueueController` and set `useWinRTPointerInput = false`. The dispatcher
is **not** load-bearing:

1. The compositor (`wuc::Compositor()`) is created by the shared `WinRTCompositor`
   singleton at `Graphics/Graphics/D2DFactory-Windows.cpp:90`. Creating a
   `Compositor` on a thread requires a `DispatcherQueue` to already exist on it.
2. That queue is created **centrally** at event-loop start:
   `Core/Threads/EventLoop-Windows.cpp:133` calls `Threads::initMainThread()`
   (`Core/Threads/ThreadUtils-Windows.cpp:39`), torn down at `EventLoop-Windows.cpp:180`.
3. In `Window::Native`'s constructor the order is
   `initializeComposition()` **then** `initializePointerInput()` (`:101-102`).
   Composition (which calls `getWinRTCompositor()`) is fully set up *before* the
   local dispatcher is created — so the local dispatcher cannot be what enables it.
4. `EmbeddedView-Windows.cpp` hosts the exact same composition stack
   (`getWinRTCompositor()` → `DesktopWindowTarget` → `ContainerVisual`) with **no
   local dispatcher at all** — direct proof one isn't needed per-window.

⇒ Removing `initializePointerInput()` entirely introduces no new failure mode. If
a `Window` were ever constructed before the event loop started, composition would
already be broken today (the central dispatcher wouldn't exist yet), so this
change doesn't regress that case.

### Exact removals

Delete each of the following from `Window-Windows.cpp`:

- **Includes (top):** `<winrt/Windows.System.h>` (`:11`), `<winrt/Windows.UI.Core.h>`
  (`:12`), `<winrt/Windows.UI.Input.h>` (`:13`), `<DispatcherQueue.h>` (`:18`).
  Keep `<winrt/Windows.UI.Composition.Desktop.h>` (`:14`), the interop headers
  (`:16-17`), and `<winrt/Windows.Graphics.Display.h>` (`:15`) is removed by A1.
- **Namespace aliases:** `namespace wucore = ...` (`:41`) and `namespace wui = ...`
  (`:42`). Keep `namespace wuc` (`:40`).
- **Ctor:** the `initializePointerInput();` call (`:102`).
- **Dtor:** the whole block (`:110-118`):
  ```cpp
  if (inputSource && useWinRTPointerInput)
  {
      inputSource.PointerPressed(pointerPressedToken);
      inputSource.PointerReleased(pointerReleasedToken);
      inputSource.PointerMoved(pointerMovedToken);
  }
  inputSource = nullptr;
  dispatcherController = nullptr;
  ```
  (Keep the `rootVisual`/`target` teardown that follows.)
- **Method:** `initializePointerInput()` (`:209-229`) in full.
- **Declarations:** the pointer-handler decls + their comment (`:309-315`).
- **Definitions:** `onPointerPressed` (`:392-416`), `onPointerReleased`
  (`:418-443`), `onPointerMoved` (`:445-461`).
- **Members:** `inputSource` (`:323`), `dispatcherController` (`:326`),
  `pointerPressedToken`/`pointerReleasedToken`/`pointerMovedToken` (`:328-330`),
  `useWinRTPointerInput` (`:331`).
- **`windowProc` guards:** the five `if (self->useWinRTPointerInput) break;`
  pairs at `:560-561, :585-586, :607-608, :642-643` (leave the message handling
  bodies that followed them intact).

Net: ~100 lines removed; the file's input story becomes single-path and honest.

**Verify:** Windows build; full mouse interaction smoke test (click, drag,
wheel, move) in the GUI app and a WebView app, since these messages all flow
through the now-simplified `windowProc`.

---

## A3. Extract a shared composition-host-window helper

**Where:** new file(s) under `Lib/eacp/Graphics/Window/`, consumed by
`Window-Windows.cpp` and `EmbeddedView-Windows.cpp`.

> Do A1 and A2 first — the line references below describe the pre-A1/A2 layout;
> after those edits the duplication is cleaner to factor.

**Problem:** the two files are the same composition-hosted-HWND machinery,
copy-pasted with cosmetic renames. Duplicated pieces:

| Piece | `Window-Windows.cpp` | `EmbeddedView-Windows.cpp` |
|---|---|---|
| VK code constants | `VK` (`:21`) | `VKEmbedded` (`:15`) |
| LPARAM coord extract | `getXFromLParam`/`getYFromLParam` (`:31`) | `…Embedded` (`:24`) |
| window-class registration | `:131` | `:65` |
| `initializeComposition` | `:183` | `:106` (differ only in the `CreateDesktopWindowTarget` topmost flag: `1` vs `0`) |
| `setContentView` | `:336` | `:138` |
| `ensureAllLayersRendered` | `:370` | `:179` (identical) |
| keyState + `onKeyDown`/`onKeyUp`/`getModifiers` | `:271` | `:195` |
| `windowProc` body (paint/size/mouse/key) | `:463` | `:228` (near-identical) |

The `MouseEvent`-from-`LPARAM` block is reproduced ~6 times across the two files.

The duplication has **already caused feature drift**: `EmbeddedView`'s `windowProc`
has no `WM_MOUSEWHEEL`, no `SetCapture`/`ReleaseCapture` drag tracking, and no
drag-vs-move distinction — all of which `Window` has (`Window-Windows.cpp:557-667`).
An embedded view silently can't scroll or drag-track. A shared WndProc fixes this
for free.

### Proposed structure

Add `Lib/eacp/Graphics/Window/CompositionHostWindow-Windows.h` (+ `.cpp` for the
out-of-line bodies; keep one definition for unity safety). It owns the shared
state and behavior:

```cpp
namespace eacp::Graphics
{
// Shared base for the two composition-hosted HWND surfaces (top-level Window and
// child EmbeddedView). Owns the HWND, the DesktopWindowTarget + root visual, the
// content-view plumbing, DPI, keyboard state, and the common WndProc handling.
struct CompositionHostWindow
{
    // topMost: passes the CreateDesktopWindowTarget isTopmost flag (Window=1,
    // EmbeddedView=0). className/style/parent let each surface create its HWND.
    void createHostWindow(const wchar_t* className, DWORD style, HWND parent,
                          int physicalWidth, int physicalHeight, void* selfForWndProc);
    void initializeComposition(bool topMost);
    float getDpiScale() const;                 // GetDpiForWindow — see A1
    void attachContentView(View* view);        // setBounds + InsertAtTop + ensureAllLayersRendered
    void ensureAllLayersRendered(const View* view) const;

    ModifierKeys getModifiers() const;
    void onKeyDown(uint16_t vk);
    void onKeyUp(uint16_t vk);

    // Handles the messages common to both surfaces (WM_SIZE, WM_PAINT,
    // WM_ERASEBKGND, WM_*BUTTON*, WM_MOUSEMOVE, WM_MOUSEWHEEL, WM_KEYDOWN/UP),
    // dispatching to contentView. Returns nullopt for messages it doesn't own so
    // the caller's WndProc can handle surface-specific ones first/after.
    std::optional<LRESULT> handleCommonMessage(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd = nullptr;
    View* contentView = nullptr;
    wuc::Desktop::DesktopWindowTarget target {nullptr};
    wuc::ContainerVisual rootVisual {nullptr};
    std::bitset<256> keyState;
};
} // namespace eacp::Graphics
```

Each surface keeps only what's genuinely its own:

- **`Window::Native`** — `quitCallback`; `WM_CLOSE` → `quitCallback()`;
  `WM_DESTROY` (the no-`PostQuitMessage` comment); `WM_DPICHANGED`
  (`SetWindowPos` + rescale rootVisual); the content-view→HWND registry
  (`registerContentViewHwnd`); `toFront()`; `setTitle()`; the
  `SetProcessDpiAwarenessContext` call. Its static `windowProc` checks these
  surface-specific messages, then delegates the rest to
  `host.handleCommonMessage(...)`, then `DefWindowProcW`.
- **`EmbeddedView::Native`** — `setSize()` and the child-window style. Its
  `windowProc` just delegates to `handleCommonMessage` then `DefWindowProcW`.

### Important considerations

- **WebView host lookup parity.** `findHostHwndForView()` (used by
  `WebView-Windows.cpp:39` and `Window-Windows.cpp:76`) resolves a content-view
  root → host HWND via the `contentViewToHwnd` map, which **only `Window`
  populates**. A `WebView` embedded inside an `EmbeddedView` currently can't find
  its host HWND. If the shared `attachContentView` performs the registration for
  both surfaces, this latent gap closes — but that's a **behavior change**;
  decide deliberately and test a WebView-in-EmbeddedView case. (`EmbeddedViewDemo`
  app exists under `Apps/Graphics/EmbeddedViewDemo` — good test bed.)
- **WM_NCCREATE self-binding** stays in each static `windowProc` (it binds the
  `self`/`host` pointer via `GWLP_USERDATA`); only the post-bind dispatch is shared.
- **Unity safety:** put the registry (`contentViewToHwnd`, `registerContentViewHwnd`,
  etc.) and any shared free functions in a single `.cpp`. Do not leave them
  defined in `Window-Windows.cpp` if `EmbeddedView` now needs them too — one
  definition only.
- Keep `getWinRTCompositor()` / `findHostHwndForView()` as the existing cross-TU
  externs.

### Suggested steps

1. Create the header + `.cpp`; move the VK constants, LPARAM helpers,
   `ensureAllLayersRendered`, keyState/modifiers, composition init, DPI, and the
   `MouseEvent`-from-`LPARAM` construction into it.
2. Build the shared `handleCommonMessage` from `Window`'s richer `windowProc`
   (the one with wheel + capture + drag), so `EmbeddedView` gains those.
3. Rewrite `Window::Native` and `EmbeddedView::Native` as thin owners of a
   `CompositionHostWindow`, keeping only surface-specific messages/members.
4. Decide the host-HWND-registry parity question above.
5. `clang-format`; build `-DEACP_UNITY_BUILD=OFF`; smoke-test GUI + EmbeddedView +
   WebView apps; then build once with `-DEACP_CI_BUILD=ON` to confirm unity links.

**Risk:** medium — this is the one item that genuinely needs a Windows compiler in
the loop. Expect a couple of iterations on the WndProc delegation split.

---

# Part B — WebView shared-code consolidation

## B1. De-duplicate the WebView registry

`registeredWebViews()` + `registerWebView` + `unregisterWebView` exist twice:
`WebView.mm:585-608` (in `detail::` / anon) and `WebView-Windows.cpp:75-89`
(anon). Only the *lookup* (`findFocusedWebView()` on Apple, `focused()` on
Windows) is truly platform-specific.

**Fix:** move the vector + register/unregister into `WebView-Shared.cpp`, exposed
through a `detail::registeredWebViews()` accessor declared in a shared header
(`WebViewPlatform-Apple.h` already declares it for Apple — generalize the
declaration so Windows uses it too). Keep `focused()`/`findFocusedWebView()` per
platform. All access is main-thread, so no locking needed (preserve that
assumption).

## B2. De-duplicate zoom constants + step wrappers

`minZoomLevel`/`maxZoomLevel`/`zoomStep` and `zoomIn`/`zoomOut`/`resetZoom` are
**byte-for-byte identical** in `WebView.mm:711-729` and
`WebView-Windows.cpp:71-73, 1863-1876`. The clamp in `setZoom` is duplicated too.

**Fix:** move the three constants and a `clampZoom(double)` helper into
`WebView-Shared.cpp` (or a small shared header). Implement `zoomIn/zoomOut/
resetZoom` once there in terms of the platform `getZoom()/setZoom()`. Each
platform keeps only the native `setZoom`/`getZoom` body and calls `clampZoom`.

## B3. Share the streaming-response decision layer

The range *math* (`resolveRangeHeader`/`contentRangeValue`) is already shared in
`WebView-Shared.cpp` — good. But the layer above it (choose 200/206/416, build
`Content-Range`/`Content-Length`/`Accept-Ranges`) is re-derived in both
`WebView.mm:445-503` (`beginStreamingTask`) and `WebView-Windows.cpp:790-857`
(`handleStreamingRequest`).

**Fix:** add a shared helper to `WebView-Shared.cpp`:
```cpp
struct StreamingResponsePlan
{
    int statusCode;
    Vector<std::pair<std::string, std::string>> headers; // name -> value
    ByteRange served;
    bool hasBody;
};
StreamingResponsePlan planStreamingResponse(std::string_view rangeHeader,
                                            const StreamingResource& resource);
```
Each backend then only translates the plan to its native response type
(`NSHTTPURLResponse` / `ICoreWebView2WebResourceResponse`). Keep the CORS
`Access-Control-Allow-Origin: *` header decision in the plan so it stays
consistent across platforms.

**Risk:** medium — exercised by `Tests/WebView/StreamingRangeTests.cpp` and
`StreamingPumpTests.cpp`; run them.

## B4. Replace hand-rolled JSON with `Miro::Json`

`WebView-Windows.cpp` hand-rolls `unwrapJsonString` (`:95-177`) and
`extractJsonStringField` (`:1095-1127`). `Miro::Json::parse`/`print` is already a
dependency of this layer (`WebView.h:6` includes `<Miro/Miro.h>`; `Bridge.cpp`
uses it throughout) and is linked (`WebView/CMakeLists.txt:14`).

The hand-rolled versions are also lossy: `extractJsonStringField` copies the char
after a backslash verbatim (`\n` → `n`, `\t` → `t`), and `unwrapJsonString`'s
`\u` path is BMP-only by its own comment (`:153`).

**Fix:** in the `WebMessageReceived` handler (`:973-1003`), parse the envelope
with `Miro::Json::parse` and read `name`/`body` as fields. For
`evaluateJavaScript`'s result unwrapping (`:1671`), use `Miro::Json` to decode a
JSON string result instead of `unwrapJsonString`. Delete both hand-rolled
functions.

**Watch:** the bridge `body` is itself a JSON-encoded string handed to the
message handler; preserve that contract (parse the outer envelope, hand the inner
`body` string through unchanged). Covered by `Tests/WebView/CommandDispatchTests.cpp`,
`WebViewCallTests.cpp`, `AsyncBridgeTests.cpp` — run them. If there's a deliberate
reason to keep Miro out of this TU, add a comment; but it's already linked, so the
default should be to use it.

**Risk:** medium — it's on the bridge message path; lean on the existing tests.

---

# Part C — Smaller best-practice items

## C1. Remove `NSLog` from the macOS drag hot path

**Where:** `Lib/eacp/WebView/WebView/WebViewPlatform-macOS.mm:203-205`
```cpp
NSLog(@"[eacp drag] mouseDragged threshold crossed, firing drag (%d paths)",
      armedPaths.size());
```
Unconditional console logging on every file-drag start in release builds. Delete
it (or gate behind a debug macro). **This one is build-verifiable on macOS.**

## C2. Dead members / API

- `Window::Native::ownerWindow` (`Window-Windows.cpp:93, :317`) is stored but
  never read — remove the member and its ctor-init.
- `EmbeddedView::onResizeRequest` (`EmbeddedView.h:27`) is declared but invoked by
  **no** backend (neither macOS nor Windows). Decide: either wire it (call it from
  the resize path) or remove it. Removing is the smaller change; wiring it is the
  better feature if embedded hosts are expected to react to size changes.

---

# Part D — Cross-platform parity gaps (decisions, not just cleanup)

These are cases where a public option/contract is honored on macOS but silently
dropped on Windows. Each is a feature addition — flag for a product decision
rather than folding blindly into the cleanup:

- **`WindowOptions::onResize` / `onWillResize`** — wired on macOS
  (`Window-macOS.mm:60-86`); never invoked on Windows (`WM_SIZE`,
  `Window-Windows.cpp:505`, updates bounds without calling them).
- **`WindowOptions::minWidth/minHeight`** — applied on macOS (`setContentMinSize`,
  `Window-macOS.mm:201`); Windows never handles `WM_GETMINMAXINFO`, so the minimum
  is ignored. The header comment reads as cross-platform.
- **`WebView::mouseExited` → WebView2 `LEAVE`** — `WebView-Windows.cpp:1381`
  expects a leave event, but `Window-Windows.cpp`'s mouse path never calls
  `TrackMouseEvent` / handles `WM_MOUSELEAVE`, so it can't fire and the WebView's
  hover/`:hover` state can stick when the cursor leaves. (Naturally addressed if
  the shared A3 WndProc adds `WM_MOUSELEAVE` tracking.)

If you implement A3, adding `onResize` dispatch and `WM_MOUSELEAVE` tracking into
the shared WndProc is the natural place; `minWidth/minHeight` needs a
`WM_GETMINMAXINFO` handler on the top-level `Window` only.

---

# What's already good (don't "fix" these)

- The `detail::` platform-interface headers (`WebViewPlatform-Apple.h`,
  `Snapshot-Apple.h`) — clean seam letting macOS/iOS share `WebView.mm` while
  isolating the few real differences.
- `resolveRangeHeader` / `contentRangeValue` in `WebView-Shared.cpp` — genuinely
  shared HTTP logic; the model B3 should extend.
- WebView2 teardown ordering with the rationale spelled out
  (`WebView-Windows.cpp:322-369`) and the `~WebView` no-double-close note.
- The drag-from-genuine-event design and its explanation
  (`WebViewPlatform-macOS.mm:26-35`).
- `JsStringLiteral.h`'s inline-in-header ODR note — the pattern to follow for any
  new shared helper under unity builds.

---

# Recommended order & verification checklist

1. **C1** (macOS, trivial) — verify on this macOS host.
2. **A1** (Windows, trivial) — verify on Windows.
3. **A2** (Windows, small) — verify on Windows; mouse smoke test.
4. **A3** (Windows, large) — the main refactor; needs Windows compiler iteration.
   Decide the host-HWND-registry parity question. Smoke-test GUI + EmbeddedView +
   WebView; build `-DEACP_CI_BUILD=ON` to confirm unity links.
5. **B1, B2** (small reuse wins) — run WebView tests.
6. **B3, B4** (medium) — run the streaming + bridge/dispatch tests.
7. **D** items — only if in scope; best landed inside A3's shared WndProc.

For each step: `clang-format` edited files, build `-DEACP_UNITY_BUILD=OFF`, run
relevant tests, then a final unity build (`-DEACP_CI_BUILD=ON`).
