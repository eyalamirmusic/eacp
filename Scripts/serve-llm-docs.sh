#!/usr/bin/env bash
# Serves build/agent-docs rendered as HTML with working links, at http://localhost:8642
set -euo pipefail
docs_dir="$(cd "$(dirname "$0")/.." && pwd)/build/agent-docs"
port="${1:-8642}"
(until curl -sf -o /dev/null "http://localhost:${port}/index.md"; do sleep 1; done; open "http://localhost:${port}/index.md") &
exec npx --yes markserv --port "$port" --livereloadport false --browser false "$docs_dir"
