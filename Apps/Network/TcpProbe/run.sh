#!/usr/bin/env bash
# Demos the probe both ways: first against a dead port - the refusal verdict
# should arrive in milliseconds, not after the connect timeout - and then
# against a live TcpServer.
#
#   Apps/Network/TcpProbe/run.sh [build-dir]   (default build dir: ./build)
set -euo pipefail

repo="$(cd "$(dirname "$0")/../../.." && pwd)"
build="${1:-$repo/build}"

cmake --build "$build" --target TcpServer TcpProbe >/dev/null

# macOS produces a .app bundle; Linux a plain executable. Find whichever.
bin() {
    local app="$build/Apps/Network/$1/$1.app/Contents/MacOS/$1"
    local plain="$build/Apps/Network/$1/$1"
    [ -x "$app" ] && echo "$app" || echo "$plain"
}

echo "--- nothing listening on 5050: expect a refusal in milliseconds ---"
"$(bin TcpProbe)" --port 5050 || true

"$(bin TcpServer)" &
server=$!
trap 'kill "$server" 2>/dev/null' EXIT
sleep 0.5

echo "--- TcpServer now up on 5050: expect open ---"
"$(bin TcpProbe)" --port 5050
