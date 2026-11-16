#!/usr/bin/env bash
# Runs the WebViewTests that Scripts/run-webview-tests-on-windows.sh built on a
# Windows box inside the ssh user's logged-in desktop session (a scheduled
# task), since WebView2 needs a desktop, and copies the log back.
#
#   Scripts/run-webview-tests-in-desktop-session.sh <ssh host> <local log path>
#   Scripts/run-webview-tests-in-desktop-session.sh jamie@tamby-windows x64.log
set -euo pipefail

host="$1"
log="$2"

encoded="$(printf '%s' '
$home = $env:USERPROFILE
$repo = Join-Path $home "projects\eacp-webview-windows-health"
$runLog = Join-Path $home "eacp-webview-tests.log"
$doneFlag = Join-Path $home "eacp-webview-tests.done"
$batch = Join-Path $home "eacp-webview-tests.bat"
Set-Content -Encoding ascii $batch "@echo off`r`ncd /d $repo`r`nbuild-webview\Tests\WebView\WebViewTests.exe > $runLog 2>&1`r`necho done > $doneFlag"
Remove-Item -ErrorAction SilentlyContinue $runLog, $doneFlag
schtasks /Create /TN eacp-webview-tests /TR $batch /SC ONCE /ST 00:00 /RU $env:USERNAME /IT /F 2>$null | Out-Null
schtasks /Run /TN eacp-webview-tests | Out-Null
$deadline = (Get-Date).AddMinutes(30)
while (!(Test-Path $doneFlag) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 5 }
schtasks /Delete /TN eacp-webview-tests /F | Out-Null
Get-Content $runLog
' | iconv -f UTF-8 -t UTF-16LE | base64 | tr -d '\n')"

ssh -o ServerAliveInterval=30 "$host" "powershell -NoProfile -EncodedCommand $encoded" > "$log" 2>/dev/null
grep -E "passed|FAIL " "$log" | tail -40
