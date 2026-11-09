param([string]$BuildDir = "build")

$ErrorActionPreference = "Stop"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$visualStudio = & $vswhere -latest -property installationPath
$vcvars = Join-Path $visualStudio "VC\Auxiliary\Build\vcvars64.bat"

cmd /c "`"$vcvars`" >nul && cmake -G Ninja -B $BuildDir -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl >nul && cmake --build $BuildDir --target GPUDeviceLossTests"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& (Join-Path $BuildDir "Tests\GPU\GPUDeviceLossTests.exe")
$testExitCode = $LASTEXITCODE
Write-Output "GPUDeviceLossTests exit code $testExitCode"
exit $testExitCode
