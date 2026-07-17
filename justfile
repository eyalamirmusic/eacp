# Build, run and install the wim terminal.
#
# Windows recipes run under cmd with the MSVC environment chained in via
# vcvarsall. The 8.3 short path keeps every recipe line quote-free — just's
# argument quoting and cmd's disagree about embedded double quotes.

set windows-shell := ["cmd.exe", "/c"]

vcvars := 'C:\PROGRA~1\MICROS~1\18\COMMUN~1\VC\Auxiliary\Build\vcvarsall.bat'

[private]
default:
    @just --list

# Configure the build directory (Ninja, Debug, unity off for LSP).
[macos]
configure:
    cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF

[windows]
configure:
    call {{ vcvars }} arm64 >nul && cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF

# Build the terminal (and its daemon).
[macos]
build:
    @test -f build/CMakeCache.txt || just configure
    cmake --build build --target CowTerm

[windows]
build:
    @if not exist build\CMakeCache.txt just configure
    call {{ vcvars }} arm64 >nul && cmake --build build --target CowTerm

# Build and launch the terminal.
[macos]
run: build
    open "build/Apps/Terminal/Terminal/Terminal.app"

[windows]
run: build
    start build\Apps\Terminal\Terminal\Terminal.exe

# Build and install: /Applications/wim.app (macOS),
# %LOCALAPPDATA%\Programs\wim (Windows).
[macos]
install: build
    ditto "build/Apps/Terminal/Terminal/Terminal.app" "/Applications/wim.app"
    @echo "installed /Applications/wim.app"

[windows]
install: build
    if not exist %LOCALAPPDATA%\Programs\wim mkdir %LOCALAPPDATA%\Programs\wim
    copy /y build\Apps\CowTerm\CowTerm\CowTerm.exe %LOCALAPPDATA%\Programs\wim >nul
    copy /y build\Apps\CowTerm\CowTerm\CowTermDaemon.exe %LOCALAPPDATA%\Programs\wim >nul
    @echo installed to %LOCALAPPDATA%\Programs\wim
