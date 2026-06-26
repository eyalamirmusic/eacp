# TrayApp — Claude-Desktop-style launcher

## The brief (from the user)

Make a cross-platform tray-icon app that launches a borderless window like
Claude Desktop's quick-launch bar.

- Tray icon, always on in the background, app stays open.
- Global hotkey launches the window. **The key command must be configurable.**
  (User mused "ctrl + space + L"; a system hotkey is modifiers + ONE key, so
  Space+L can't be a single chord. Default to Ctrl+L, but make it one config
  line at the top of the app.)
- Window is a single text box matching the screenshot (dark panel, placeholder
  "What can I help you with today?").
- On submit: **swallow the text and dismiss the window.** Do nothing else.
  (Notifications were explicitly dropped from scope — do NOT add them.)
- Requests launch-at-login. **Enable by default on first run** — it's a demo
  app, keep it dead simple, no opt-in UI.
- macOS first, structured so Windows can follow (stub the Windows side).

### Working style the user demanded
- Be a USER of the framework. Copy-paste from existing examples (esp. the
  existing TrayApp). Only add a framework feature IFF you actually need it.
- Do NOT read the whole repo / "get a PhD in the framework." Read examples,
  copy, and only dig into a specific header when a feature is missing.

## What already exists (reuse, do not rebuild)

- `Apps/Graphics/TrayApp/Main.cpp` — already a working tray-only app:
  - `Apps::setDockIconVisible(false)` + `LSUIElement` in `Info.plist.in`.
  - Borderless rounded panel via `WindowOptions`: `WindowFlags::Borderless`,
    `cornerRadius`, `alwaysOnTop`, `visibleOnAllWorkspaces`, `showInactive`,
    `isPrimary=false`.
  - `Window::setVisible(bool)` / `isVisible()` keep the window alive across
    toggles; `togglePanel()` flips it. `Window::toFront()` raises + activates.
  - `TrayIcon`: `setIcon(Image)`, `setTooltip`, `setMenu(Menu)`,
    `setOnClick(Callback)` (Windows left-click; on macOS the menu owns clicks).
  - `Menu` / `MenuItem::withAction(name, Callback)`, `addSeparator()`,
    `Apps::quit()`.
  - The example builds its tray icon procedurally (no asset file needed).

- `Lib/eacp/Graphics/Widgets/TextInput.h` — the text box. This is the swallow
  target. Key API:
  - ctor `TextInput(const FontOptions&)` or `TextInput(const std::string&)`.
  - `setPlaceholder`, `setText`, `getText`, `setCursorPosition`.
  - `setTextColor`, `setBackgroundColor`, `setBorderColor`, `setPadding`.
  - `onSubmit(std::function<void(const std::string&)>)` — fires on Enter.
  - `focus()` (inherited from View) to make it accept typing.
  - Usage example: `Apps/Graphics/Todo/Main.cpp` (TodoItemView) — shows
    add/removeSubview, `textInput.focus()`, and `onSubmit` wiring.

- `View` (`Lib/eacp/Graphics/View/View.h`): `keyDown(const KeyEvent&)`,
  `focus()`, `hasFocus()`, `addChildren`, `addSubview`/`removeSubview`,
  `getLocalBounds`, `setBounds`, `scaleToFit`, `setHandlesMouseEvents`,
  `setGrabsFocusOnMouseDown`.

- Keyboard (`Lib/eacp/Graphics/Graphics/Keyboard.h`):
  - `KeyCode::` virtual codes (these ARE the macOS virtual keycodes, e.g.
    `L = 0x25`, `Space = 0x31`, `Escape = 0x35`).
  - `struct ModifierKeys { bool shift, control, alt, command; }`.
  - `struct KeyEvent { ... uint16_t keyCode; ModifierKeys modifiers; ... }`.

- `FontOptions` (`Lib/eacp/Graphics/Primitives/Font.h`):
  `FontOptions().withName("Helvetica-Bold").withSize(14.f)`.

- `Callback = std::function<void()>` (`Lib/eacp/Core/Utils/Common.h`);
  Pimpl pattern in `Lib/eacp/Core/Utils/Pimpl.h`.

## What is MISSING and must be added (only what's needed)

### 1. Global hotkey — REQUIRED, configurable
No global/system-wide hotkey exists. There is window-scoped `keyDown` and
`Keyboard::isKeyPressed`, but nothing that fires while the app is unfocused.

Add a small feature mirroring the `TrayIcon` shape (public header +
`Pimpl<Native>` + per-platform `.mm`/`.cpp`):

- `Lib/eacp/Graphics/HotKey/GlobalHotKey.h`
- `Lib/eacp/Graphics/HotKey/GlobalHotKey-macOS.mm`
- `Lib/eacp/Graphics/HotKey/GlobalHotKey-Windows.cpp` (stub)

API (configurable by design):
```cpp
class GlobalHotKey {
public:
    GlobalHotKey(ModifierKeys modifiers, uint16_t keyCode, Callback onPressed);
    ~GlobalHotKey();
    // (or a setHotKey(...) setter — either way the combo is a param)
};
```

macOS impl: Carbon `RegisterEventHotKey` + `InstallApplicationEventHandler`
for `kEventHotKeyPressed`. Convert `ModifierKeys` → Carbon flags
(`controlKey`/`optionKey`/`cmdKey`/`shiftKey`); our `KeyCode` values are
already macOS virtual keycodes so no keycode translation is needed. Keep a
static id→callback registry in the .mm (the Carbon handler is C). Fires on the
main thread. **Carbon hotkeys need no Accessibility permission** (unlike a
CGEventTap) — prefer Carbon.

CMake: `Lib/eacp/Graphics/CMakeLists.txt` already links `-framework Carbon`
and `-framework Cocoa` in the APPLE/non-IOS branch. Just add the source files
to the right `target_sources` branches and the include to
`Lib/eacp/Graphics/Graphics.h`.

### 2. Launch-at-login — REQUIRED, on by default
No login-item API exists. Add (Core/App, mirroring existing split):
- `Lib/eacp/Core/App/LoginItem.h` — e.g. `Apps::setLaunchAtLogin(bool)` /
  `isLaunchAtLogin()`.
- `LoginItem-macOS.mm`: `SMAppService.mainApp` register/unregister (macOS 13+).
  Needs `-framework ServiceManagement` added to the Core APPLE/non-IOS link
  list. Requires running from a bundle (TrayApp is a bundle).
- `LoginItem-Windows.cpp` (stub).
- Call `setLaunchAtLogin(true)` unconditionally in the app ctor (idempotent).

### NOT in scope
- **Notifications: dropped.** Do not add NSUserNotification / UNUserNotification.

## App changes (`Apps/Graphics/TrayApp/Main.cpp`)
Start from the existing file and:
1. Replace the static-label `PanelView` with one containing a `TextInput`
   filling the panel: dark bg `{0.11,0.11,0.13}`, placeholder
   "What can I help you with today?".
2. `textInput.onSubmit([this](auto&){ textInput.setText(""); hide(); })` —
   swallow + dismiss. Add `Esc` in `keyDown` to dismiss too.
3. Add a `GlobalHotKey` member built from a config at the top of the file
   (default `ModifierKeys{.control=true}` + `KeyCode::L`). Its callback shows
   the window, `toFront()`, and `textInput.focus()`; toggles off if visible.
4. In the ctor call `Apps::setLaunchAtLogin(true)`.
5. Keep tray menu: Toggle, Quit.

## Build / verify
```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF
cmake --build build --target TrayApp
# build/Apps/Graphics/TrayApp/TrayApp.app
```
Sign with the dev identity (see memory: screen-recording-tcc-signing) so the
login item / any TCC grants persist across rebuilds:
`-DEACP_CODESIGN_IDENTITY="Apple Development: Jamie Pond (QU2A8WQL89)"`.

## Layout conventions learned
- New sources are added by hand to the module `CMakeLists.txt` under the right
  `target_sources(...)`, inside the matching `APPLE`/`IOS`/`WIN32` branch.
- Public class = header + `struct Native; Pimpl<Native> impl;` + one impl file
  per platform. `TrayIcon` is the canonical template to copy.
- Aggregator headers (`Graphics/Graphics.h`, `Core/Core.h`) pull in the public
  headers — add new ones there.
