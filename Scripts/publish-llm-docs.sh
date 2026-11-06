#!/usr/bin/env bash
# Uploads build/llm-docs (markdown and the HTML rendered from it) to
# gs://$EACP_DOCS_BUCKET/<sha>/, then mirrors that tree to each alias directory,
# deleting whatever the alias held before.
#
# Usage: Scripts/publish-llm-docs.sh <short-sha> [alias...]
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <short-sha> [alias...]" >&2
    exit 2
fi
sha=$1
shift

repo_root=$(cd "$(dirname "$0")/.." && pwd)
bucket=gs://${EACP_DOCS_BUCKET:-eacp-docs}
site_dir=$repo_root/build/llm-docs
alias_cache_control="public, max-age=300"

if [[ ! -f $site_dir/index.md || ! -f $site_dir/index.html ]]; then
    echo "missing $site_dir/index.{md,html}: run Scripts/generate-llm-docs.py first" >&2
    exit 1
fi

if gcloud storage ls "$bucket/$sha/index.html" > /dev/null 2>&1; then
    echo "$bucket/$sha/ already exists; sha directories are never rewritten"
else
    gcloud storage rsync --recursive "$site_dir" "$bucket/$sha"
    gcloud storage objects update "$bucket/$sha/**.md" \
        --content-type="text/markdown; charset=utf-8" > /dev/null
    gcloud storage objects update "$bucket/$sha/llms.txt" \
        --content-type="text/plain; charset=utf-8" > /dev/null
fi

for alias in "$@"; do
    gcloud storage rsync --recursive --delete-unmatched-destination-objects \
        "$bucket/$sha" "$bucket/$alias"
    gcloud storage objects update "$bucket/$alias/**" \
        --cache-control="$alias_cache_control" > /dev/null
done
