#!/usr/bin/env bash
# Runs the TcpClient against the local TcpServer: builds both, starts the
# server in the background, points the client at it, prints the round-trip,
# then cleans up.
#
#   Apps/Network/TcpClient/run.sh [build-dir]   (default build dir: ./build)
set -euo pipefail

repo="$(cd "$(dirname "$0")/../../.." && pwd)"
build="${1:-$repo/build}"

cmake --build "$build" --target TcpServer TcpClient >/dev/null

# macOS produces a .app bundle; Linux a plain executable. Find whichever.
bin() {
    local app="$build/Apps/Network/$1/$1.app/Contents/MacOS/$1"
    local plain="$build/Apps/Network/$1/$1"
    [ -x "$app" ] && echo "$app" || echo "$plain"
}

"$(bin TcpServer)" &
server=$!
trap 'kill "$server" 2>/dev/null' EXIT
sleep 0.5

echo "--- client ---"
"$(bin TcpClient)" --host 127.0.0.1 --port 5050
