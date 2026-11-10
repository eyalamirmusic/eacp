#!/usr/bin/env bash
# Copies this worktree to a Linux box over ssh and runs Scripts/run-webview-tests.sh
# there, which on Linux is the parser and bridge suites. Graphics is off, since
# the bridge does not link it, and libcurl is the system one, which a nix cmake
# does not search for.
#
#   Scripts/run-webview-tests-on-linux.sh <ssh host>
#   Scripts/run-webview-tests-on-linux.sh ubuntu-vm
set -euo pipefail

host="$1"
remoteDir="projects/eacp-webview-windows-health"

rsync -az --delete --exclude '/build*' --exclude '.git' ./ "$host:$remoteDir/"
linuxCmakeArgs="-DEACP_BUILD_GRAPHICS=OFF -DCURL_INCLUDE_DIR=/usr/include"
linuxCmakeArgs+=" -DCURL_LIBRARY=/usr/lib/\$(uname -m)-linux-gnu/libcurl.so"

ssh "$host" "cd $remoteDir && EACP_CMAKE_ARGS=\"$linuxCmakeArgs\" Scripts/run-webview-tests.sh build-webview"
