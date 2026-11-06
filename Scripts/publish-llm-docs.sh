#!/usr/bin/env bash
# Merges build/llm-docs (markdown) and build/llm-docs-html (Doxygen HTML) into
# one tree and uploads it to gs://$EACP_DOCS_BUCKET/<sha>/, then mirrors that
# tree to each alias directory, deleting whatever the alias held before.
# A path present in both trees is an error: nothing is overwritten silently.
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
markdown_dir=$repo_root/build/llm-docs
html_dir=$repo_root/build/llm-docs-html
site_dir=$repo_root/build/llm-docs-site
alias_cache_control="public, max-age=300"

for dir in "$markdown_dir" "$html_dir"; do
    if [[ ! -f $dir/index.md && ! -f $dir/index.html ]]; then
        echo "missing $dir: run Scripts/generate-llm-docs.py --html first" >&2
        exit 1
    fi
done

clashes=$(comm -12 \
    <(cd "$markdown_dir" && find . -type f | sort) \
    <(cd "$html_dir" && find . -type f | sort))
if [[ -n $clashes ]]; then
    echo "markdown and HTML trees both contain:" >&2
    echo "$clashes" >&2
    exit 1
fi

rm -rf "$site_dir"
mkdir -p "$site_dir"
cp -R "$html_dir/." "$site_dir/"
cp -R "$markdown_dir/." "$site_dir/"

if gcloud storage ls "$bucket/$sha/index.html" > /dev/null 2>&1; then
    echo "$bucket/$sha/ already exists; sha directories are never rewritten"
else
    gcloud storage rsync --recursive "$site_dir" "$bucket/$sha"
    gcloud storage objects update "$bucket/$sha/**.md" \
        --content-type="text/markdown; charset=utf-8" > /dev/null
fi

for alias in "$@"; do
    gcloud storage rsync --recursive --delete-unmatched-destination-objects \
        "$bucket/$sha" "$bucket/$alias"
    gcloud storage objects update "$bucket/$alias/**" \
        --cache-control="$alias_cache_control" > /dev/null
done
