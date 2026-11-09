#!/usr/bin/env bash
# Builds and runs the WebView test suites this machine can run, from the
# repository root: the parser and bridge suites everywhere, WebViewTests
# where the platform has a web view (macOS here; Windows has its own scripts).
#
#   Scripts/run-webview-tests.sh [build directory] [WebViewTests group ...]
#
# EACP_CMAKE_ARGS adds configure arguments, split on spaces.
set -euo pipefail

buildDir="${1:-build-webview}"
shift || true

cmake -G Ninja -B "$buildDir" -DCMAKE_BUILD_TYPE=Debug -DEACP_UNITY_BUILD=OFF ${EACP_CMAKE_ARGS:-} > "$buildDir.configure.log" 2>&1 \
    || { cat "$buildDir.configure.log"; exit 1; }

targets=(ScriptHostTests WebViewMessagesTests)
hasWebView=0
if ninja -C "$buildDir" -t targets all | grep -q '^WebViewTests:'; then
    targets+=(WebViewTests WebViewKeys)
    hasWebView=1
fi

cmake --build "$buildDir" --target "${targets[@]}"

"$buildDir/Tests/WebView/ScriptHostTests"
"$buildDir/Tests/WebView/WebViewMessagesTests"

if [ "$hasWebView" = 1 ]; then
    if [ "$#" -eq 0 ]; then
        "$buildDir/Tests/WebView/WebViewTests"
    else
        for group in "$@"; do
            "$buildDir/Tests/WebView/WebViewTests" --test-group "$group"
        done
    fi
fi
