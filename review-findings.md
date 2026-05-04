# eacp review findings

Persistent notes from the in-depth review covering API design, the message loop,
windowing, and the WebView. File paths/line numbers were correct as of the
review; verify before acting on any item.

Status legend: [FIXED] in current branch · [OPEN] still pending · [NIT] minor.

---

## 1. Correctness bugs

### 1.1 `ObjC::Ptr::operator==(const Ptr&)` inverted [FIXED]
`Lib/eacp/Core/ObjC/ObjC.h:71`

`operator==` returned `ptr != other.ptr`, and `operator!=` was `= default` so it
inherited the inversion. Fix: return `ptr == other.ptr` and define `!=` as
`!operator==(other)`. Pinned by `Tests/ObjC/ObjCPtrTests.mm` cases
`ptrEqualitySamePointerIsEqual` and `ptrEqualityDifferentPointersAreNotEqual`.

### 1.2 `ObjC::Ptr::isKindOfClass<A>()` ignored template parameter [FIXED]
`Lib/eacp/Core/ObjC/ObjC.h:75-78`

Hard-coded `[ptr isKindOfClass:[NSHTTPURLResponse class]]` regardless of `A`.
Fix: `[A class]`. Pinned by test `isKindOfClassReturnsTrueForCorrectClass`.

### 1.3 `ObjC::makePtr<T>()` leaked [FIXED]
`Lib/eacp/Core/ObjC/ObjC.h` (now uses `Ptr<T>{createNew<T>()}`)

`makePtr` was `attachPtr(createNew<T>())`. `createNew` returns +1 and
`attachPtr` retains, leaving the object with retain count 2 owned by a single
`Ptr` — leaked one reference on destruction. Pinned by test
`makePtrDoesNotLeak`.

### 1.4 `WebView::onTitleChanged` never fires [FIXED]
`Lib/eacp/WebView/WebView/WebView.mm:206`, `WebView.mm:52-88`

Delegate implements `observeValueForKeyPath:` for `title` but no
`addObserver:forKeyPath:options:context:` is ever called. Fix: register the
observer in `Native::Native` against `webView`'s `title` keypath, and remove
it in `~Native` (KVO is not auto-cleaned).

### 1.5 `Window` declares key-state APIs that aren't implemented on macOS [FIXED]
`Lib/eacp/Graphics/Window/Window.h:73-78`

`isKeyPressed`, `isShiftPressed`, `isControlPressed`, `isAltPressed`,
`isCommandPressed`, `getModifiers` — only `Window-Windows.cpp:617-645`
implements them. Calling on macOS fails at link time. Either implement on
macOS (track via `keyDown:`/`keyUp:` on the content view, or read
`[NSEvent modifierFlags]` for the modifier-only ones) or move out of the
cross-platform header.

### 1.6 Mouse drag/up gets re-routed by hit testing [FIXED]
`Lib/eacp/Graphics/View/View.cpp:137-178`,
`Lib/eacp/Graphics/View/View-macOS.mm:106-138`

`dispatchMouseEvent` always hit-tests `event.pos` to pick a target, including
for `Dragged`/`Up`. AppKit captures the mouse on the originating NSView, but
the C++ target gets re-picked each event — drags off the originating view
silently route to the wrong View or `nullptr`. Fix: store the `Down` target
and route subsequent `Dragged`/`Up` to it until `Up`. The existing
`mouseDownPosition` stores position only — extend to capture the target.

### 1.7 Windows `EventLoop::quit()` won't wake `GetMessage` [OPEN]
`Lib/eacp/Core/Threads/EventLoop-Windows.cpp:68-71`

Just flips an atomic; the loop is parked in `GetMessage`. Cross-thread
`quit()` hangs. Fix: post a thread message
(`PostThreadMessageW(threadId, WM_NULL, 0, 0)`) or `PostQuitMessage(0)`.

---

## 2. Message loop & application lifecycle

### 2.1 Redundant quit chain [FIXED]
`Lib/eacp/Core/App/App.cpp:11-25`, `Window-macOS.mm:15-18`,
`Window-macOS.mm:169-172`

User close → `windowWillClose:` → `Apps::quit()` → async destroyApp +
stopEventLoop → `~Window` runs and calls `options.onQuit()` *again*. Remove
the destructor call to `onQuit`.

### 2.2 `WindowOptions::onQuit = Apps::quit` default is a footgun [FIXED]
`Lib/eacp/Graphics/Window/Window.h:40`

Closing any popup window kills the app. `Apps/EmbeddedPopups/Main.cpp:33` and
`Apps/WebViewEmbed/Main.cpp:29` already work around this. Replace with an
opt-in `Window::isPrimary` flag, or have only the first window default to
quit-on-close.

### 2.3 `EventLoop::quit()` and `getApp()` lack thread guards [OPEN]
`EventLoop-macOS.mm:7-36`. Add `assertMainThread()` to `quit` (and document
that `getApp()` lazy-init must run on main).

### 2.4 `NSApplicationActivationPolicyRegular` set unconditionally [OPEN]
`EventLoop-macOS.mm:9-13`. Pops a Dock icon for headless apps (e.g. the
`Console` example). Make it configurable.

### 2.5 `[NSApp activateIgnoringOtherApps:YES]` runs in every `Window` ctor [OPEN]
`Window-macOS.mm:120`. Steals focus when popups are created. Restrict to the
first window or expose as a `WindowOptions` toggle.

### 2.6 `quit()` fake-event trick is undocumented [NIT]
`EventLoop-macOS.mm:25-35`. `[NSApp stop:nil]` only takes effect after the
next event is dequeued — that's why a synthetic `NSEventTypeApplicationDefined`
is posted. One-line comment would save the next reader.

---

## 3. Windowing API

### 3.1 `Window` API surface is extremely thin [OPEN]
No `setSize`, `setPosition`, `setMinSize/MaxSize`, `close`, `requestFocus`,
`setFullScreen`, `setIcon`. Only `setTitle` post-construction. Add at minimum
`setBounds(x, y, w, h)` / `getBounds()`.

### 3.2 `WindowFlags` enum is misnamed [OPEN]
`Window.h:11-25, 55`. Used as `std::vector<WindowFlags>` but called "Flags".
Either switch to `enum class WindowStyle : uint32_t` with bitwise ops and a
sane default mask, or rename the type to match the storage.

### 3.3 `Window` options are mostly write-once [NIT]
Style mask, title, size, centering all happen in the ctor. There's no path to
mutate flags later. Document this — or expose mutators.

### 3.4 `Window::getHandle()`/`getContentViewHandle()` leak NSObjects as `void*` [NIT]
Acceptable for an interop boundary, but consider an opaque `NativeHandle`
wrapper.

### 3.5 `Window::setContentView` swap path is silent [NIT]
`Window-macOS.mm:128-133`. AppKit handles the swap, but trackers/layers on
the old view that depend on `superview` may misbehave. Worth a one-line note.

### 3.6 No keyboard delivery at the `Window` level [OPEN]
Keys flow to the content view's first responder — including a WKWebView,
which keeps them. No `Window::keyDown`/`onKeyDown` hook exists for app-wide
shortcuts (other than the menu bar).

---

## 4. View / native bridging

### 4.1 Dual hierarchies — native subviews + C++ tree [OPEN]
`View-macOS.mm:106-138, 261-271`. Each child View has a real NSView added via
`addSubview:`. AppKit dispatches mouseDown to whichever NSView is hit, but
`dispatchMouseEvent:` re-routes to root and re-hit-tests through the C++
tree. The native NSView hierarchy is decorative for mouse handling and
duplicates work. Either (a) make subview NSViews opt out of mouse handling
(their `hitTest:` returns the root) or (b) commit to native-view-as-source-of-
truth and dispatch from each NativeView to its own `cppView`. Related to bug
1.6.

### 4.2 `Pimpl<T>` is `shared_ptr` but copy-deleted [FIXED]
`Lib/eacp/Core/Utils/Pimpl.h:17-30`. Move-only semantics with shared_ptr
storage — pay the atomic refcount for nothing. Switch to `unique_ptr` (declare
`~Pimpl()` in a `.cpp` if incomplete-type issues arise).

### 4.3 `WebView::impl` bypasses `Pimpl<T>` [FIXED]
`WebView/WebView.h:117` uses raw `std::shared_ptr<Native>`. Inconsistent with
View/Window. Harmonize on Pimpl, or document the popup-ctor reason.

### 4.4 WebView async callbacks capture raw `WebView*` [FIXED]
`WebView.mm:175-217, 246-249`. `eacp::Threads::callAsync([owner = _owner, ...]() { owner->onX(...); })`
is a use-after-free if the WebView is destroyed before the block runs. Fix:
capture `std::weak_ptr<Native>` (you already use `shared_ptr<Native>`) and
`lock()` in the block. Bonus: WK delegate callbacks already arrive on main —
the redundant `callAsync` only widens the racy window. The
`evaluateJavaScript` completion handler at `WebView.mm:559-560` is the worst
offender (already on main).

### 4.5 `WebView::focused()` only matches by key window [OPEN]
`WebView.mm:488-511`. With multiple WKWebViews in one window, returns the
first registered. Use `[NSApp keyWindow].firstResponder` (or
`wkWebView.window.firstResponder`) to disambiguate.

### 4.6 `WKURLSchemeHandler` is fully synchronous [OPEN]
`WebView.mm:285-323`. Blocks the WebKit content thread during provider
invocation; `stopURLSchemeTask` is a no-op. OK for ResEmbed-backed
resources; pitfall for disk/network. Consider an async variant:
`std::function<void(std::string_view, ResourceCallback)>`.

### 4.7 `developerExtrasEnabled` uses a private WKPreferences key [OPEN]
`WebView.mm:64-68`. Widely used but unsupported. Gate behind a debug build
option if App Store distribution matters. The public `inspectable` setter
right below is the supported alternative on macOS 13.3+.

### 4.8 iOS zoom hack uses non-standard CSS [NIT]
`WebView.mm:469-477`. `document.documentElement.style.zoom` is non-standard;
won't propagate through nested iframes or composited transforms reliably.
Comment why this approach was chosen.

### 4.9 Popup `Native` config-retain semantics are non-obvious [NIT]
`WebView.mm:97`: `config = ObjC::Ptr {init.configuration, ObjC::RetainMode {}};`
— retains because WebKit hands the configuration in unowned. Add a one-line
comment so a future reader doesn't "fix" it.

---

## 5. ObjC interop

### 5.1 `ObjC::Ptr` ownership semantics are surprising [OPEN]
`ObjC.h:14-18`. `Ptr(T*)` does *not* retain; `Ptr(T*, RetainMode)` does. The
default ctor assumes a +1 transfer. This is the opposite of most ObjC C++
smart pointers (Chromium's `base::scoped_nsobject` retains by default and has
an `assume_ownership` adopt). An autoreleased pointer passed to the default
ctor double-frees. Consider renaming for clarity:
`Ptr<T>::adopt(T*)` for +1 transfer, `Ptr<T>::retain(T*)` for unretained
input. Or factory functions.

### 5.2 `CFRef::reset` doesn't null-out before reassign [NIT]
`Lib/eacp/Core/ObjC/CFRef.h:25-29`. Self-reset `r.reset(r.get())` would
release-then-release. Clear `ref` after release.

### 5.3 `Ptr::operator=` triggers clang-tidy self-assignment warning [NIT]
False positive — `reset(other.ptr)` is self-safe (`if (ptr != other)`). Add
`if (this == &other) return *this;` to silence the linter explicitly.

---

## 6. Threading

### 6.1 `Singleton<EventLoop>` and `Singleton<AppHandle>` lock in single instance [OPEN]
Type-keyed Meyers singletons. Limits testing, secondary modal loops,
multi-window IPC. Note for evolution.

### 6.2 `DisplayLink` uses deprecated `CVDisplayLink` [OPEN]
`Helpers/DisplayLink-macOS.mm:7`. Deprecated since macOS 14 in favor of
`NSView.displayLink` / `CADisplayLink`. Migration will require an API change
(per-view linkage).

### 6.3 `EventLoop::call(Callback func)` signatures inconsistent [NIT]
Header takes by value, free `callAsync` takes `const Callback&`. Pick one.

---

## 7. App template / `run<T>()`

### 7.1 No way to pass options to `T` [OPEN]
`Apps::run<T>()` requires `T` default-constructible. Want a runtime config?
Make it `run<T>(args...)` with perfect-forwarding into `App<T>`.

### 7.2 No way to retrieve the user's `T` from `getGlobalApp()` [NIT]
Returns `AppBase*`. If deliberate (encourage encapsulation), document.

---

## 8. Smaller observations

- `pathFromURL` (`WebView-Shared.cpp:26-45`) strips `?` queries but not
  `#` fragments — strip both.
- `mimeForPath` (`WebView-Shared.cpp:5-24`) misses `.gif`, `.webp`, `.ico`,
  `.wasm`, `.mp4`, `.mp3`, `.woff` (only `.woff2`), `.txt`.
- `WebView::loadHTML`/`loadURL` use `[NSString stringWithUTF8String:...]`
  directly instead of `Strings::toNSString` — inconsistent.
- `EmbeddedView` (`EmbeddedView-macOS.mm`) doesn't expose key/mouse state.
- `Menu-macOS.mm` `installedBar()` static lifetime: replacing the menu bar
  mid-construction can drop a `MenuAction` that captures `this`. Edge case.
- `Window::getContentViewHandle` returns the live `[window contentView]` —
  correct (AppKit may swap), worth a comment.
- `View::resized()` is defined twice: empty inline body in `View.h:69` and
  again in `View.cpp:27`. Pick one.
- `View::dispatchMouseEvent` doesn't synthesize Enter/Exit when the hovered
  view is removed mid-hover.
- `WindowOptions::width/height` are content-size in points but not documented;
  `onResize`/`onWillResize` already say so — extend to `width/height`.
- `Lib/eacp/WebView/WebView.h` is a thin forwarding include
  (`#include "WebView/MenuHelpers.h"`); the actual API lives at
  `Lib/eacp/WebView/WebView/WebView.h`. Reorganize the public header.

---

## 9. Top recommended next fixes (priority order)

1. `WebView::onTitleChanged` KVO registration (1.4).
2. macOS `Window` keyboard-state methods — implement or remove from header
   (1.5).
3. Mouse capture for drag/up (1.6 + 4.1).
4. Use-after-free protection in WebView async callbacks (4.4).
5. Default `WindowOptions::onQuit` (2.2) + remove redundant `~Window` quit
   (2.1).
6. Switch `Pimpl<T>` to `unique_ptr` (4.2) and harmonize WebView (4.3).
