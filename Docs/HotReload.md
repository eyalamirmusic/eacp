# Hot reload

Hot reload rebuilds part of a running app when you save a source file, and
swaps the new code in without restarting the app. A layout is the usual case:
you change a margin, save, and a second later the window moves its widgets. A
view is the other: you change a shader, or swap one view class for another,
and the running window shows the new one.

The code that reloads lives in a library of its own, which
`eacp_add_hot_reload` creates. It is a short list of functions, named with
`EACP_RELOADABLE`, and the app reaches them through `Plugins::Reloadable`.

- **Static mode**, which covers every release build and every platform hot
  reload does not run on, makes that library a static library linked into
  the app. The app calls its own copies and nothing else.
- **Dev mode** makes it a module, a dynamic library the app loads when its
  `Reloadable` is constructed. The app contains none of that code. On every
  save the module is rebuilt and loaded again.

## Using it: `Reloadable` and `EACP_RELOADABLE`

### Listing the functions

Put the functions in the library's sources, in an anonymous namespace. List
them once with `EACP_RELOADABLE`, at global scope:

```cpp
// Layout.cpp
#include "LayoutABI.h"
#include <eacp/Core/Plugins/ReloadableEntry.h>

namespace
{
void demoLayout(const LayoutTargets& targets, const eacp::UI::Rect& bounds)
{
    auto area = bounds.inset(16.f);
    targets.title.setBounds(area.removeFromTop(36.f));
    // ...
}
} // namespace

EACP_RELOADABLE(demoLayout)
```

The app finds each function by its type, so each type may appear only once:
`EACP_RELOADABLE(makeDemoView, demoLayout)` is fine, but two factories of the
same type are a compile error. `EACP_RELOADABLE` may also appear only once
per app.

### Calling a function

Declare the function's type in a header that both sides include:

```cpp
// LayoutABI.h
using LayoutFunction = void(const LayoutTargets& targets,
                            const eacp::UI::Rect& bounds);
```

Then hold a `Reloadable` in the app and fetch the function by its type
whenever you call it:

```cpp
// Main.cpp
#include <eacp/Core/Plugins/Reloadable.h>

eacp::Plugins::Reloadable reloadable;

void applyLayout() { reloadable.call<LayoutFunction>(targets, getLocalBounds()); }
```

That is the minimal form. It needs no options, no symbol names and no
`if constexpr`. In dev mode it loads the module, watches the library's source
directory, rebuilds the module on save, checks it and swaps it in, logging
each step through `LOG`. In static mode it calls the app's own copy.

- `function<F>()` returns the current generation's `F*`.
- `call<F>(args...)` calls it.

Fetch the function each time you use it. A pointer kept across a reload
points into a module that is about to be unloaded.

### Making objects

For an object, list a factory that returns a raw `T*`:

```cpp
// Views.cpp
#include <eacp/Core/Plugins/ReloadableEntry.h>
#include <eacp/GPU/GPU.h>

namespace
{
struct PlasmaView final : eacp::GPU::GPUView { /* ... */ };

eacp::Graphics::View* makeDemoView()
{
    return new PlasmaView;
}
} // namespace

EACP_RELOADABLE(makeDemoView)
```

`create<T>()` calls the current generation's `T*()` factory and returns the
object in an `OwningPointer<T>`. The caller owns it. `Reloadable` owns no
objects, so one `Reloadable` can make any number of them, of one type or of
several. When `T` is a base type, the object is deleted through `T*`, so `T`
then needs a virtual destructor, as in ordinary C++.

### When there is no value

An empty `OwningPointer` is how `create<T>()` says "nothing", the same in both
modes:

- **The factory returns null.** `create<T>()` returns an empty pointer and
  logs `HotReload: factory returned nothing`.
- **The factory throws.** The exception is caught, `HotReload: factory threw:
  <what>` is logged, and `create<T>()` returns an empty pointer.
- **No module is loaded**, because the first load in dev mode failed. The
  failure is logged once. Until a build loads one, `create<T>()` returns an
  empty pointer, `function<F>()` returns `nullptr`, and `call<F>()` calls
  nothing and returns a value-initialised result (nothing for `void`).

A type that `EACP_RELOADABLE` does not list is a programming error, not a
missing value: `create<T>()` and `function<F>()` throw `std::logic_error`
naming the signature.

`OptionalView::set` with an empty pointer is the same as `clear()`, so
`content.set(reloadable.create<View>())` is always safe.

### The lifetime rule

`onReloaded()` runs once at the end of the `Reloadable`'s constructor, in
static mode as well as dev mode, and again after every reload. Your setup
therefore lives in one place: the hook that makes the objects at start-up is
the same hook that replaces them on a reload.
`ReloadableOptions::callOnReloadedAtCreation` (default `true`) turns the
first call off.

- **Declare the `Reloadable` last**, after every member its `onReloaded`
  uses. Its constructor calls the hook, so those members must already exist.
- **On a reload, replace or destroy every object** made by `create<T>()`
  inside `onReloaded`. That is the one moment when the new module is loaded
  and the old one, which made those objects, is still loaded.
  `Plugins::unload` unloads the old module one loop turn later, and an object
  still alive then would run its destructor from code that is no longer
  there.
- **Destroying the `Reloadable` is safe for members destroyed after it.**
  Members are destroyed in reverse order, so the holders declared before the
  `Reloadable` die after it. Its module is not unloaded there and then: it
  goes one loop turn later, or, when no loop is running (as at the end of
  `Apps::run`), it stays loaded until the process exits.

### Views: `Graphics::OptionalView`

`Graphics::OptionalView<T = View>` (`Graphics/View/OptionalView.h`) is a
general-purpose view that holds at most one child, and it is the natural home
for a reloaded view:

- `set(content)` destroys the previous child, adds the new one as a subview
  and makes it fill the `OptionalView`. An empty pointer just clears it.
- `clear()` destroys the child.
- `get()`, `operator*`, `operator->`, `explicit operator bool` and
  `hasContent()` read it.
- `resized()` keeps the child filling the view.

A destroyed `View` removes itself from its parent (every platform's
`View::~View` calls `removeFromParent()`), so the old view leaves the tree on
its own and nothing has to be detached. `Apps/GPU/HotReload` is the whole
pattern:

```cpp
struct DemoRoot final : Graphics::View
{
    DemoRoot() { addChildren({content}); }

    Plugins::ReloadableOptions reloadOptions()
    {
        auto options = Plugins::ReloadableOptions {};
        auto reinstall = [this]
        { content.set(reloadable.create<Graphics::View>()); };
        options.onReloaded = reinstall;
        return options;
    }

    void resized() override { content.setBounds(getLocalBounds()); }

    Graphics::OptionalView<> content;
    Plugins::Reloadable reloadable {reloadOptions()};
};
```

`reinstall` is the only place a view is made. It runs once when `reloadable`
is constructed, which is why `reloadable` comes after `content`, and again
on every reload. There, `set` makes the new view and then destroys the old
one, while the old module is still loaded. When `DemoRoot` is destroyed,
`reloadable` goes first and `content` after it, and the module that made the
view stays loaded until then.

### Options and hooks

`Reloadable` takes an optional `Plugins::ReloadableOptions`. Every member has
a working default, so you set only the members you need.

| Member | Default | Called |
| --- | --- | --- |
| `log(std::string_view)` | `LOG` | for each line below |
| `onReloaded()` | no-op | once at construction, then after each new module is loaded, while the old one still is |
| `callOnReloadedAtCreation` | `true` | whether `onReloaded()` also runs at construction |
| `onStateChanged()` | no-op | whenever `describeState()` changes, and once after the first load in dev mode |
| `onBuildFailed(const BuildFailure&)` | no-op | after a failed build |
| `onLoadFailed(const std::string&)` | no-op | after a module failed to load or was refused |
| `onBuildOutput(std::string_view)` | no-op | for each chunk of build output |
| `sources` | the target's | watcher settings: pattern, poll interval, settle time |
| `loop` | from the target | a whole `HotReloadLoop::Options`, replacing the target's |

`Reloadable` always writes to `log` first and then calls the hook. Setting a
hook therefore never silences the log line; to silence logging, set `log` to a
no-op. The lines are:

```
HotReload: watching <source files, or the source directory>
HotReload: generation 0
HotReload: building <module>…
HotReload: build failed: <summary>
HotReload: load failed: <reason>
HotReload: generation <N>
HotReload: factory returned nothing
HotReload: factory threw: <what>
```

Static mode logs only the factory lines, and calls no hook but
`onReloaded()`.

`sources` applies to the options taken from the app's target. If its
`directory` or `files` is empty, the target's is kept. `loop` replaces
everything the target would give. Tests and custom builds use it to point
`Reloadable` at their own build command and module. Whichever is used,
`Reloadable` sets `requiredSymbol` and `validate` itself; a `validate` you set
in `loop` runs after its own check.

`describeState()` returns the status line (`generation N`, `building…`,
`build failed: …` or `load failed: …`), or `static` in static mode.
`getGeneration()` is 0 for the first module that loads and goes up by one
with each reload. A `Reloadable` can be neither copied nor moved.

### What happens on a reload

When a build succeeds, the new module is staged and opened. Before it replaces
the current one, two checks run:

1. The module must export the table `EACP_RELOADABLE` defines.
2. The table must provide every signature that generation 0's table has.

If either check fails, the module is refused. The previous generation stays,
and the reason becomes `load failed: …`, naming what is wrong in the
compiler's own spelling of the type (Clang's below; GCC writes
`ReloadableTestObject*()` and `long int()`):

- a missing factory: `the module lacks ReloadableTestObject *()`
- a changed type: changing `int answer()` to `long answer()` gives
  `signature mismatch: the module lacks int () and has long ()`

Without these checks, the app would call a function through the wrong type.
A table may add a type generation 0 did not have; restart the app to use it.

If both checks pass:

1. The new module becomes the current generation, and
   `HotReload: generation N` is logged.
2. `function<F>()` and `create<T>()` now reach the new module.
3. `onReloaded()` is called. This is where the app replaces its objects.
4. The old module is unloaded one loop turn later.

A failed build, a module that does not load, a missing table and a refused
table all keep the previous generation. Each one is logged, passed to its
hook and shown by `describeState()`. Nothing builds until the first save.

If generation 0 itself fails to load (the module was never built, say), the
failure is logged and passed to `onLoadFailed`, `describeState()` says
`load failed: …`, and the "no module is loaded" rules above apply. The first
build that loads becomes generation 0.

## The module rules

In dev mode the module links nothing. Its references to eacp and to every
other library it uses stay undefined and are resolved when it loads, against
what the app exports. Both therefore share one eacp, built by one compiler,
and C++ types can cross between them. The rules follow from that.

- **Only the table is exported.** `EACP_RELOADABLE` defines
  `eacpReloadableTable`, an `extern "C"` function marked
  `EACP_HOT_RELOAD_EXPORT`, which returns every listed function with its
  signature. Everything else in the library's sources stays in an anonymous
  namespace, so that two generations never share a symbol.
- **One per app.** `eacp_add_hot_reload` creates one library, and the table
  has a fixed name. A second `EACP_RELOADABLE` in the same app fails to link
  with a duplicate symbol, which is intended. List every function in one
  `EACP_RELOADABLE`.
- **One function per type.** Lookup is by type, so `EACP_RELOADABLE`
  `static_assert`s that the listed functions' types are pairwise distinct.
- **The module owns nothing.** It installs no callbacks, starts no threads or
  timers, keeps no statics the app can see and registers nothing with the
  app. Its code is unloaded one loop turn after the next reload, and anything
  still pointing into it would then point at unloaded code.
- **Objects die inside `onReloaded`.** Anything the module creates is handed
  to the app, which destroys or replaces it inside the next `onReloaded` at
  the latest.

The table is a C++ struct, `ReloadableTable` (a pointer and a count of
`ReloadableEntry {function, signature}`), returned from a C-linkage function.
This is safe because the module and the app share a compiler.
`ReloadableEntry.h` turns off clang's `-Wreturn-type-c-linkage` around the
entry, and `eacp_add_hot_reload` sets `EACP_HOT_RELOAD_ABI` on the app and
the library, which turns that warning off for any other exported entry point
they write. The entries live in a local static of that non-inline function,
so two images' copies are never merged by the loader.

## CMake: `eacp_add_hot_reload` and `EACP_HOT_RELOAD`

```cmake
eacp_add_app(HotReload Main.cpp)
target_link_libraries(HotReload PRIVATE eacp-ui)

eacp_add_hot_reload(HotReload HotReloadLayout SOURCES Layout.cpp [HOT_RELOAD <bool>])
target_link_libraries(HotReloadLayout PRIVATE eacp-ui)
```

`eacp_add_hot_reload(<app> <library> [SOURCES <files...>] [HOT_RELOAD
<bool>])` (`CMake/EacpPlugin.cmake`) creates `<library>`. Configure it like
any other target: `target_sources`, `target_include_directories`,
`target_compile_definitions` and `target_link_libraries`, `PRIVATE` or
`PUBLIC`, before or after the call. Whether the app is in dev mode or static
mode is decided like this:

- `HOT_RELOAD` takes any CMake boolean (`ON`/`OFF`, `1`/`0`, `TRUE`/`FALSE`,
  `YES`/`NO`, or a variable holding one, as in `HOT_RELOAD ${MY_OPTION}`),
  read the way `if()` reads it. A true value is always dev mode, a false one
  always static, whatever the option says.
- With no `HOT_RELOAD`, or one whose value is empty (an unset variable, say),
  the app follows the `EACP_HOT_RELOAD` cache option (default `OFF`), read the
  same way.
- iOS, Android and Windows are always static, whatever either says.

Both in-tree examples pass `HOT_RELOAD ON`, so on macOS and Linux they are in
dev mode in every build directory, and in static mode on Windows, iOS and
Android:

- `Apps/UI/HotReload` (`HotReload`, library `HotReloadLayout`) reloads a
  layout function.
- `Apps/GPU/HotReload` (`HotReloadGPU`, library `HotReloadGPUViews`) reloads a
  factory for a `GPUView`. The app holds the view in an `OptionalView` and
  replaces it inside `onReloaded`.

A configure prints `<app>: hot reload ON` or `<app>: hot reload OFF`.

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF \
      -DEACP_HOT_RELOAD=ON
```

**Static mode** makes `<library>` a `STATIC` library, links the app to it,
and defines `EACP_HOT_RELOAD=0` on the app. The library's dependencies reach
the app the usual way. No module, watcher or build runner is built, and
`Reloadable` uses the table the app links (`eacpReloadableTable`, which the
`Reloadable` constructor refers to, so the linker takes it from the archive).

**Dev mode** does the following:

- It makes `<library>` a `MODULE` and makes the app depend on it, so the
  module exists before the app runs. The app does not link it and contains
  none of its code.
- **It routes the library's dependencies through the app.** At the end of the
  configure, once every `target_link_libraries` has run, everything the
  library links is linked into the app instead, and the module keeps each
  target only as `$<COMPILE_ONLY:…>`: its include directories, definitions
  and options, with no link. So eacp, and any static library the module
  uses, exists once, in the app.
- It defines `EACP_HOT_RELOAD=1` and the module's path, the CMake executable,
  the build directory, the configuration, the library's target name, its
  source directory and its source files on the app.
  `Plugins::hotReloadTarget()` reads them back.
- It gives the app `ENABLE_EXPORTS`: `-export_dynamic` on Apple, and
  `--export-dynamic` on Linux, which keeps the exports alive under the
  libraries' Release IPO.
- It links every static library in the app's link closure whole-archive, the
  library's dependencies included, so the module can call code the app itself
  never calls.
- It links the module with `-undefined dynamic_lookup` on Apple, which is
  already a Linux module's default. The module is position-independent, built
  with no Release IPO and, under GCC, with `-fno-gnu-unique`.
- It compiles every library in that closure at default visibility. That
  setting belongs to the library targets, so it applies to the whole build
  directory. `eacp_add_plugin` modules built there re-export eacp and lose
  their isolation, so do not mix the two in one build directory.

Only the library's own sources are watched: the files in the target's
`SOURCES`, made absolute at generate time, so sources added later with
`target_sources` count too. A header is watched only if it is listed there;
saving the app's `Main.cpp` beside it rebuilds nothing. A library with no
sources falls back to watching its whole directory. A save rebuilds the
module alone
(`cmake --build <dir> --config <config> --target <library>`), never the
running app.

`Plugins/HotReload.h` holds the mode (`Plugins::isHotReload()`) and
`hotReloadTarget()`. It and `Plugins/Reloadable.h` are included directly,
never through the `Core.h` umbrella, because what they contain depends on the
app's own compile definitions. `Reloadable` itself is a plain class built
into eacp-core, which never sees those definitions or the app's table. Only
its public constructor is a template, `template <bool hotReload =
isHotReload()>`, so the mode and the module are read at the app's own call
site and handed to the class as a `Detail::ReloadableSetup`. In static mode
that setup carries the app's table; in dev mode it does not refer to the
table at all, since the app has none. Tests build the setup by hand: their
own table, `true` or `false`, and a `loop` in the options.

### Limits of the dependency routing

- **Prebuilt static libraries.** An `IMPORTED` static library is linked into
  the app but is neither whole-archived nor given default visibility, so the
  module can call only what the app already uses and the library exports.
  Libraries built from source in the same build, CPM's included, work.
- **Link items that are not targets** (a path, a `-l` flag, a generator
  expression) go to the app, and give the module nothing to compile against;
  use a target for anything whose headers the module needs.
- **A library linked by several apps** is not possible: each
  `eacp_add_hot_reload` creates its own library, with its own name.
- **Code in the library that holds a `Reloadable` itself** is not supported:
  the mode and module come from the app's compile definitions, which the
  library does not have.

## Platform limits

Hot reload runs on macOS and Linux. Everywhere else it is static:

- **Windows.** `WINDOWS_EXPORT_ALL_SYMBOLS` never exports a static library's
  symbols, so a module has no eacp of the app's to bind to. A module that
  linked the executable instead would relink the locked, running exe on every
  save. `EACP_HOT_RELOAD_EXPORT` is empty there.
- **iOS and Android.** The app runs on a device or simulator with no CMake to
  run and no source tree to watch.

## The lower layers

`Reloadable` is built from four pieces in `eacp-core`. Each can be used on its
own, for a module with its own entry points or a build that is not CMake's.
All of them run on the main thread only.

### `Plugins::HotReloadLoop` (`Plugins/HotReloadLoop.h`)

A `Files::SourceWatcher`, a `Processes::BuildRunner` and a `ReloadableLibrary`
combined into one object. It is built from `Options`, or from a
`HotReloadTarget` and a required symbol. `hotReloadOptions(target)` returns
the `Options` a target describes, and `hotReloadBuildCommand(target)`
(`Plugins/HotReloadTarget.h`) returns the build command alone.

- Each settled burst of changes starts a build. A change during a build
  causes exactly one more run, reported as a build of its own.
- A successful build reloads the module and calls `onReloaded(library)`. That
  happens only when the module exports `Options::requiredSymbol` and
  `Options::validate` accepts it, so `onReloaded` can look its entry point up
  with no fallback.
- A failed build or load keeps the generation and calls `onBuildFailed` or
  `onLoadFailed`.
- `onBuildStarted`, `onBuildOutput` and `onStateChanged` report the rest.
- `getState()` and `describeState()` (`generation N`, `building…`,
  `build failed: …`, `load failed: …`) drive a status line.
- The module's directory and the staging directory are left out of the watch
  when they lie inside the sources.
- Nothing builds until the first change. `rebuild()` starts a build by hand.
- No callback may destroy the loop.

### `Plugins::BuildFailure` and `buildFailureFrom`

These hold the build's output, whether it launched, whether it exited, its
exit code and a one-line `summary`. To pick the summary line,
`buildFailureFrom` strips ANSI colour from the output and takes the first
match from this list, in order:

1. a compiler error with a file location (clang, GCC or MSVC)
2. any `error:` line that is not the driver's or the linker's
3. a `CMake Error`
4. ninja's `FAILED:` line
5. the last line of output

With no output at all, the summary names the cause instead: the build did
not start, it was cancelled, or it exited with the given code.

### `Plugins::ReloadableLibrary` (`Plugins/ReloadableLibrary.h`)

A `DynamicLibrary` over a module file that is rebuilt while the app runs.

- **Staged copies.** `reload()` copies the built file to
  `<stem>-<gen>-<pid>-<serial>` in a staging directory and opens that copy,
  never the built file itself. A new path for each load avoids the registry
  handing back the old image, and keeps the built file free for the linker to
  overwrite. Two running copies of an app never remove each other's staged
  files.
- **The old image.** It goes to `Plugins::unload`, which unloads it one loop
  turn later when a loop is running. Every pointer that came from it is
  invalid after the next successful reload.
- **Destruction.** The current image is not unloaded in the destructor
  either. It goes one loop turn later, or, with no loop running, stays
  loaded until the process exits. Objects it made can therefore outlive the
  library for the rest of the current turn. Its staged copy is removed at
  destruction where the OS allows deleting a loaded file; Windows does not, so
  there it is removed on that later turn, and with no loop running it stays.
- **Refusals.** A module without `requiredSymbol` is refused with
  `lastError()` set to `no <symbol>`. A module that `validate` answers with a
  non-empty string is refused with that string as `lastError()`. Either way,
  the current generation stays loaded.
- **Accessors.** `getGeneration()`, `getLoadedPath()`, `findFunction` and
  `findSymbol`.

### `Processes::BuildRunner` (`Process/BuildRunner.h`)

Runs one command again on demand, on a background thread, with stderr merged
into stdout. `onOutput` and `onFinished` are called on the main thread. A
`run()` during a run asks for exactly one more run. A cancelled run still
finishes, with `exited == false`. Cancelling kills only the direct child, not
ninja under cmake.

### `Files::SourceWatcher` (`Utils/SourceWatcher.h`)

A `Files::FileWatcher` over one source tree. It is always recursive and
matches `Files::sourceFilePattern` (C, C++ and Objective-C sources and
headers) unless its `Options` name another pattern. The rest of
`FileWatcherOptions` goes in `Options::watcher`: poll interval, settle time
and excluded directories. A directory that does not exist yet is watched until
it appears. `Options::files`, when not empty, watches exactly those files
instead of the directory.
