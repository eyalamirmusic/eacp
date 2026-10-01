# Running eacp on an Android phone

For a Windows, macOS or Linux machine and an Android 13+ phone with Vulkan 1.3.
Every command runs from the eacp checkout, in any shell: PowerShell, cmd, zsh or bash.

## 1. Install

CMake 3.31 or later, Ninja and Git, on the `PATH`. Nothing else.

## 2. Set up the SDK, once

```
cmake -P Scripts/android-setup.cmake
```

It puts the SDK, NDK and a JDK in `~/.eacp/android` (about 3 GB). It ends by printing the paths of `adb` and `ndk-stack`, written `<adb>` and `<ndk-stack>` below.
Run it again after an update; it is quick when nothing is missing.

## 3. Connect the phone

1. Settings > About phone > Software information: tap Build number seven times.
2. Settings > Developer options: turn on USB debugging.
3. Plug it in, unlock it, accept "Allow USB debugging", and check it shows as `device`:

```
<adb> devices
```

## 4. Run HelloGPU

```
cmake --preset android
cmake --build --preset android --target HelloGPU-run
```

## 5. Make your own app

Create these three files verbatim (here for an app called `HelloWorld`), then fill in your own code where marked.

1. `Apps/Android/HelloWorld/CMakeLists.txt`:

```cmake
eacp_add_app(HelloWorld Main.cpp) # fill me in: more .cpp files
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

4. Run it: the phone turns green and the log line prints in the terminal.

```
cmake --build --preset android --target HelloWorld-run
```

## When it goes wrong

- "the phone is locked": unlock it; the app is behind the lock screen.
- "has not allowed USB debugging": accept the prompt on the phone, run again.
- "installing ... for the first time": answer the Play Protect prompt on the phone, if one comes.
- "signed with another debug key": nothing to do; the old install, and its data, is removed.
- Logs: `<adb> logcat -s eacp`
- A native crash, symbolicated:

```
<adb> logcat -d | <ndk-stack> -sym build-android/Apps/Android/HelloWorld
```
