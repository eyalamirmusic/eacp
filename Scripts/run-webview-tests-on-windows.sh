#!/usr/bin/env bash
# Applies this worktree (its diff from eyal/develop, uncommitted and untracked
# files included) to a clone on a Windows box over ssh, then builds and runs the
# WebView test suites there.
#
#   Scripts/run-webview-tests-on-windows.sh <ssh host> <vcvars batch file>
#   Scripts/run-webview-tests-on-windows.sh jamie@tamby-windows vcvars64.bat
#   Scripts/run-webview-tests-on-windows.sh windows-vm vcvarsarm64.bat Group/ ...
# Each further argument is a WebViewTests group prefix to run. BASELINE=1 runs
# the same on the branch's base without the patch.
set -euo pipefail

host="$1"
vcvars="$2"
shift 2
groups="$*"
remoteDir="projects/eacp-webview-windows-health"
patchName="eacp-webview-windows-health.patch"

base="$(git merge-base HEAD eyal/develop)"
patch="$(mktemp)"
trap 'rm -f "$patch"' EXIT
git diff --binary "$base" > "$patch"
git ls-files --others --exclude-standard -z \
    | xargs -0 -n1 git diff --binary --no-index /dev/null >> "$patch" || true

scp -q "$patch" "$host:$patchName"
scp -q Scripts/build-webview-tests-windows.bat "$host:build-webview-tests-windows.bat"

# Encoded so neither git-bash nor cmd.exe on the far side rewrites the script.
remoteScript="
if (!(Test-Path $remoteDir)) { git clone -q https://github.com/eyalamirmusic/eacp $remoteDir }
cd $remoteDir
git fetch -q origin
git checkout -q -f $base
git clean -fdq -e build-webview -e '*.log'
if ('${BASELINE:-0}' -ne '1') { git apply ../../$patchName; if (\$LASTEXITCODE -ne 0) { exit 1 } }
cmd /c ..\\..\\build-webview-tests-windows.bat $vcvars
if (\$LASTEXITCODE -ne 0) { exit 1 }
\$failed = 0
foreach (\$group in '$groups'.Split(' ', [StringSplitOptions]::RemoveEmptyEntries)) {
  & build-webview\\Tests\\WebView\\WebViewTests.exe --test-group \$group
  if (\$LASTEXITCODE -ne 0) { \$failed = 1 }
}
exit \$failed
"
encoded="$(printf '%s' "$remoteScript" | iconv -f UTF-8 -t UTF-16LE | base64 | tr -d '\n')"

ssh "$host" "powershell -NoProfile -EncodedCommand $encoded"
