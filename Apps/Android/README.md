# Running eacp on Android

For a macOS, Windows or Linux machine, and an Android device or emulator image
at or above the app's minimum API level (`EACP_ANDROID_MIN_SDK`, below) with
Vulkan 1.1. Building, installing, running and debugging go
through Android Studio and Gradle, as they do for any Android app; CMake
compiles the library, as it does on every platform.

## 1. Install

- Android Studio. In its SDK Manager (Settings > Languages & Frameworks >
  Android SDK > SDK Tools, with "Show Package Details" ticked) install "NDK
  (Side by side)". eacp builds with the version `-DEACP_ANDROID_NDK_VERSION`
  locks to, else the one `ANDROID_NDK_HOME` names, else the newest NDK there.
  The SDK, `adb` and the emulator come with Studio; `ANDROID_HOME` names
  another SDK.
- CMake 3.31 or later and Ninja, on the `PATH`. Gradle runs the CMake that ran
  the configure and looks for Ninja beside it (Homebrew keeps both in one
  place), in the SDK's CMake package, or on its own `PATH`.
- A C++ compiler for this machine, which the resource embedder's generator is
  built with: Xcode's command-line tools, Visual Studio's Build Tools, or GCC.

## 2. Configure

```
cmake -G Ninja -B build-android -DCMAKE_SYSTEM_NAME=Android
```

That finds the NDK in Studio's SDK, builds `libHelloGPU.so` for `arm64-v8a`
at the app's minimum API level (`-DANDROID_ABI=x86_64` or
`-DANDROID_PLATFORM=android-<level>` change that), which is the quick way to
check that the code compiles, and writes an Android Studio project into
`build-android/AndroidStudio` with one module per app. Every `EACP_*` and
`CPM_*` setting given here (`-DEACP_UNITY_BUILD=OFF`, a `-DCPM_Miro_SOURCE`)
reaches the project's own configures too; other `-D`s do not.

The API levels and tools are cache variables an app sets to its own values:

- `EACP_ANDROID_MIN_SDK` (default 33): `minSdk`, the oldest Android the app
  runs on and the level the configure compiles for; an `-DANDROID_PLATFORM`
  below it fails the configure. The NDK carries every level, so nothing
  installed changes it.
- `EACP_ANDROID_TARGET_SDK` (default 35): `targetSdk`.
- `EACP_ANDROID_COMPILE_SDK` (default empty): `compileSdk`. Empty means the
  newest platform installed in the SDK (SDK Manager > SDK Platforms); with
  none installed it is the target level, with a warning, and Gradle downloads
  that platform on its first sync once the SDK licenses are accepted. It may
  not be below the target level.
- `EACP_ANDROID_NDK_VERSION` (default empty): the NDK above.
- `EACP_ANDROID_GRADLE_PLUGIN` and `EACP_ANDROID_GRADLE`: the Android Gradle
  Plugin and Gradle the project runs, a pair tested together and downloaded
  on the first sync rather than installed; an app may raise them.

## 3. Run

Open the `build-android/AndroidStudio` folder in Android Studio (File > Open).
The first sync downloads Gradle and the Android Gradle Plugin, once per
machine. Pick `HelloGPU` (or `HelloNetwork`, below, or any of the `Apps/GPU`,
`Apps/UI` and `Apps/SVG` examples, which are modules of the same project) and a device: a phone with
USB debugging on (Settings > About phone, tap Build number seven times, then
Developer options > USB
debugging), or an AVD from Device Manager at or above `EACP_ANDROID_MIN_SDK`,
whose Google APIs images have a Vulkan driver on Apple Silicon and x86-64
hosts. Press Run. The app logs
under the tag `eacp` in Studio's Logcat.

To debug, if Studio's default debugger ("Detect Automatically") fails to
attach, set Run > Edit Configurations > Debugger > Debug type to "Native Only":
an eacp app has no Java for the other half to attach to.

From a terminal, in that folder, with `JAVA_HOME` at a JDK 17 or later
(Studio's own is `Android Studio.app/Contents/jbr/Contents/Home` on a Mac,
`jbr` under its install folder elsewhere):

```
./gradlew :HelloGPU:installDebug
adb shell am start -n com.eacp.hellogpu/android.app.NativeActivity
adb logcat -s eacp
```

A debug build takes environment variables from the launch intent's string
extras (`am start ... --es EACP_VK_VALIDATION 1`) and from the system property
`debug.<package>.env` (`adb shell setprop debug.com.eacp.hellogpu.env "K=V
K=V"`), which persists across launches; a release build takes none, since
anything on the device can start an activity with extras.

`./gradlew :HelloGPU:bundleRelease` makes the App Bundle Google Play takes,
once the module's `build.gradle.kts` has a `signingConfig` with your upload
key in place of the debug one it starts with.

## 4. Make your own app

Create these files (here for an app called `HelloWorld`), then fill in your
own code where marked.

1. `Apps/Android/HelloWorld/CMakeLists.txt`:

```cmake
eacp_add_app(HelloWorld Main.cpp # fill me in: more .cpp files
        DISPLAY_NAME "Hello World") # BUNDLE_ID, VERSION, VERSION_CODE, ICON,
                                    # ORIENTATION portrait|landscape,
                                    # PERMISSIONS CAMERA ...
target_link_libraries(HelloWorld PRIVATE eacp-gpu) # fill me in: eacp-text, ...
set_default_target_setting(HelloWorld)
```

2. `Apps/Android/HelloWorld/Main.cpp`:

```cpp
#include <eacp/GPU/GPU.h>

using namespace eacp;

struct HelloWorldView final : GPU::GPUView
{
    void render(GPU::Frame& frame) override
    {
        auto pass = frame.beginPass({Graphics::Color {0.1f, 0.6f, 0.3f}});
        // fill me in: draw with pass
    }

    // fill me in: update(), touchBegan(), ...
};

int main()
{
    LOG("HelloWorld: hello from eacp"); // shows in logcat under the tag eacp
    return Graphics::runWindowedApp<HelloWorldView>();
}
```

3. `Apps/Android/CMakeLists.txt`, one line below `add_subdirectory(HelloGPU)`:

```cmake
add_subdirectory(HelloWorld)
```

Run the configure again: the module appears in Studio after a sync.

`main()` runs once per activity, not once per process. Android destroys and
recreates the activity for a configuration change the manifest does not claim
(the font size, the locale) and when it reclaims a stopped app, and the
recreated one runs `main()` again on a new thread in the same process, so keep
what must survive in your app struct and let nothing assume a single run; a
`main()` that returns on its own ends the process.

Every app's manifest asks for `INTERNET`, so `eacp-network`'s HTTP client works
out of the box, over Java's own `HttpURLConnection`. Android refuses plain
`http://` to an app targeting this SDK, loopback included; configure with
`-DEACP_ANDROID_CLEARTEXT_TRAFFIC=ON` to allow it, as a local test server
needs. `WebSocket::Connection` runs over Java's own sockets, with TLS and
hostname verification for `wss://`, and the same setting governs plain
`ws://`. `HelloNetwork` is the worked example: run with that setting on, it
fetches, serves, downloads, times out and echoes over both WebSocket schemes,
logs each check under `eacp`, and turns green when all pass, red otherwise.

Any other permission is `eacp_add_app`'s `PERMISSIONS`: `PERMISSIONS CAMERA`
puts `android.permission.CAMERA` in the manifest. One the user has to grant,
as the camera is, is then asked for at run time with
`Android::requestPermission` (`<eacp/Core/Android/Permissions-Android.h>`),
which answers on the main thread once the dialog closes.

## When it goes wrong

- Studio offers to switch the project to its own SDK: that SDK has no NDK at
  the version the configure compiled with, which came from `$ANDROID_HOME` or
  `$ANDROID_NDK_HOME`. Install that NDK with Studio's SDK Manager and
  configure again.
- "Ninja not found" from Gradle: Studio started from the Dock or the Start
  menu has the login shell's `PATH` only. Put Ninja beside CMake, or install
  the SDK's CMake package, which carries one.
- A native crash: Studio's Logcat symbolicates it. From a terminal,
  `adb logcat -d | <sdk>/ndk/<version>/ndk-stack -sym <path to the unstripped
  libHelloGPU.so under the module's .cxx folder>`.
- The app is behind the lock screen: unlock the phone.
