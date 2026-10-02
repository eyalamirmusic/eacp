# Running eacp on Android

For a Windows, macOS or Linux machine, and an Android 13+ phone with Vulkan 1.1 or, with no phone, the Android Emulator the setup installs.
Every command runs from the eacp checkout, in any shell: PowerShell, cmd, zsh or bash.

## 1. Install

CMake 3.31 or later, Ninja and Git, on the `PATH`. Nothing else.

## 2. Set up the SDK, once

```
cmake -P Scripts/android-setup.cmake
```

It puts the SDK, NDK, a JDK and the Android Emulator in `~/.eacp/android` (about 8 GB, 5 of them the emulator and its system image), and an AVD called `eacp` in `~/.android/avd` (about 1 GB once booted). It ends by printing the paths of `adb` and `ndk-stack`, written `<adb>` and `<ndk-stack>` below.
Run it again after an update; it is quick when nothing is missing.
For a phone only, `cmake -DEACP_ANDROID_EMULATOR=OFF -P Scripts/android-setup.cmake` leaves the emulator out (3.3 GB).
Windows and Linux on ARM get no emulator: Google ships none for them.

## 3. Connect the phone, if you have one

No phone: skip this step; the run boots the `eacp` emulator.

1. Settings > About phone > Software information: tap Build number seven times.
2. Settings > Developer options: turn on USB debugging.
3. Plug it in, unlock it, accept "Allow USB debugging", and check it shows as `device`:

```
<adb> devices
```

## 4. Run HelloGPU

```
cmake -G Ninja -B build-android -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=CMake/AndroidToolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33
cmake --build build-android --target HelloGPU-run
```

`CMake/AndroidToolchain.cmake` is the NDK's own toolchain file, found in Android Studio's SDK, else `$ANDROID_HOME`, else where the setup put it, so the line is the same on every machine.

## 5. Make your own app

Create these three files verbatim (here for an app called `HelloWorld`), then fill in your own code where marked.

1. `Apps/Android/HelloWorld/CMakeLists.txt`:

```cmake
eacp_add_app(HelloWorld Main.cpp # fill me in: more .cpp files
        DISPLAY_NAME "Hello World") # BUNDLE_ID, VERSION, VERSION_CODE, ICON, ...
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
    LOG("HelloWorld: hello from eacp"); // shows in adb logcat -s eacp
    return Graphics::runWindowedApp<HelloWorldView>();
}
```

3. `Apps/Android/CMakeLists.txt`, one line below `add_subdirectory(HelloGPU)`:

```cmake
add_subdirectory(HelloWorld)
```

4. Run it: the phone, or the emulator, turns green and the log line prints in the terminal.

```
cmake --build build-android --target HelloWorld-run
```

## 6. Or use Android Studio

```
cmake -G Ninja -B build-android-studio/cmake -DCMAKE_TOOLCHAIN_FILE=CMake/AndroidToolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 -DEACP_ANDROID_STUDIO_DIR=build-android-studio
```

Then open the `build-android-studio` folder in Android Studio (File > Open). The first sync downloads Gradle and the Android Gradle Plugin, once per machine. Every app has a run configuration of its own, its debugger set to Native (an all-native app has no Java for Studio's default, Auto, to attach to): pick one and press Run, or Debug to stop at a breakpoint in its C++.
Every app is a module, so a new app shows up after running that line again.
From a terminal, the same project builds with `gradlew :HelloWorld:assembleDebug` inside `build-android-studio`, with `JAVA_HOME` set to a JDK 17+: the configure prints that line with the JDK it found (on a Mac, `/usr/bin/java` is a stub that fails without one installed).

## When it goes wrong

- Android Studio asks to switch the project to its own SDK: its SDK (`~/Library/Android/sdk` on a Mac, `%LOCALAPPDATA%\Android\Sdk` on Windows, `~/Android/Sdk` on Linux) has no NDK at the pinned version, so the configure took `$ANDROID_HOME` or `~/.eacp/android/sdk`. Install that NDK there with Studio's SDK Manager, or run the setup with `ANDROID_HOME` at it; the setup installs what is missing into it.

- "the phone is locked": unlock it; the app is behind the lock screen.
- "emulator eacp did not boot": start it by hand with the command it prints to see why. On Linux the emulator needs KVM: `/dev/kvm` readable by you (add yourself to the `kvm` group).
- Another AVD: `EACP_AVD=<name>` picks it over `eacp`.
- "this SDK has no emulator to boot": the setup ran with `-DEACP_ANDROID_EMULATOR=OFF`, or `ANDROID_HOME` names an SDK it did not make; run the setup again without it.
- "has not allowed USB debugging": accept the prompt on the phone, run again.
- "installing ... for the first time": answer the Play Protect prompt on the phone, if one comes.
- "signed with another debug key": nothing to do; the old install, and its data, is removed.
- Logs: `<adb> logcat -s eacp`
- A native crash, symbolicated:

```
<adb> logcat -d | <ndk-stack> -sym build-android/Apps/Android/HelloWorld
```
