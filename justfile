# eacp -- list recipes by default
default:
    @just --list

# Configure CMake (idempotent). Exports compile_commands.json for clangd.
configure:
    cmake -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    ln -sf build/compile_commands.json compile_commands.json

# Build everything
build: configure
    cmake --build build

# Build one target -- `just build-only SimpleView`
build-only TARGET: configure
    cmake --build build --target {{TARGET}}

# Build & launch an app -- `just run SimpleView` (default: SimpleView)
run TARGET="SimpleView": (build-only TARGET)
    open build/Apps/{{TARGET}}/{{TARGET}}.app

# Build & launch the SimpleView demo
simpleview: (run "SimpleView")

# List buildable apps
apps:
    @find Apps -maxdepth 1 -mindepth 1 -type d -exec basename {} \;

# Wipe build dir
clean:
    rm -rf build

# Clean rebuild
rebuild: clean build

# Format all C++/ObjC++ source via clang-format
fmt:
    git ls-files '*.h' '*.cpp' '*.mm' | xargs clang-format -i
