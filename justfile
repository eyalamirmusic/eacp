# Build, run and install the CowTerm terminal.
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

# Build CowTerm (and its daemon).
[macos]
build:
    @test -f build/CMakeCache.txt || just configure
    cmake --build build --target CowTerm

[windows]
build:
    @if not exist build\CMakeCache.txt just configure
    call {{ vcvars }} arm64 >nul && cmake --build build --target CowTerm

# Build and launch CowTerm.
[macos]
run: build
    open "build/Apps/Terminal/Terminal/CowTerm.app"

[windows]
run: build
    start build\Apps\Terminal\Terminal\CowTerm.exe

# Build and install: /Applications/CowTerm.app (macOS),
# %LOCALAPPDATA%\Programs\CowTerm (Windows).
[macos]
install: build
    ditto "build/Apps/Terminal/Terminal/CowTerm.app" "/Applications/CowTerm.app"
    @echo "installed /Applications/CowTerm.app"

[windows]
install: build
    if not exist %LOCALAPPDATA%\Programs\CowTerm mkdir %LOCALAPPDATA%\Programs\CowTerm
    copy /y build\Apps\Terminal\Terminal\CowTerm.exe %LOCALAPPDATA%\Programs\CowTerm >nul
    copy /y build\Apps\Terminal\Daemon\CowTermDaemon.exe %LOCALAPPDATA%\Programs\CowTerm >nul
    @echo installed to %LOCALAPPDATA%\Programs\CowTerm
