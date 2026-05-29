# eacp — dev recipes. Run `just` to list, `just drag` to build + run the demo.

build_dir := "build"

# List available recipes.
_default:
    @just --list

# Configure CMake (Ninja, non-unity). No-op once build/ exists. Prefers ~/projects/Miro.
configure:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -f {{build_dir}}/CMakeCache.txt ]; then exit 0; fi
    miro_flag=""
    if [ -d "$HOME/projects/Miro" ]; then
        miro_flag="-DCPM_Miro_SOURCE=$HOME/projects/Miro"
    fi
    cmake -G Ninja -B {{build_dir}} -DEACP_UNITY_BUILD=OFF $miro_flag

# Force a clean reconfigure (use after changing CMake options).
reconfigure:
    rm -rf {{build_dir}}/CMakeCache.txt {{build_dir}}/CMakeFiles
    @just configure

# Configure, build + run the native file drag-out demo (NSLog streams here).
drag: configure
    cmake --build {{build_dir}} --target WebViewDragOut
    -pkill -x WebViewDragOut
    {{build_dir}}/Apps/WebView/WebViewDragOut/WebViewDragOut.app/Contents/MacOS/WebViewDragOut
