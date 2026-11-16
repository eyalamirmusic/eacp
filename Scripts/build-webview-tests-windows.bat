@echo off
rem Builds the WebView test suites and the WebViewKeys app on Windows and runs the
rem suites that need no desktop, from the repository root.
rem
rem   build-webview-tests-windows.bat <vcvars batch file, e.g. vcvars64.bat>
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\%1" >nul || exit /b 1
cmake -G Ninja -B build-webview -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF > build-webview-configure.log 2>&1 || (type build-webview-configure.log & exit /b 1)
cmake --build build-webview --target WebViewTests ScriptHostTests WebViewMessagesTests WebViewKeys || exit /b 1
build-webview\Tests\WebView\ScriptHostTests.exe || exit /b 1
build-webview\Tests\WebView\WebViewMessagesTests.exe || exit /b 1
