#!/usr/bin/env bash
# Installs the Doxygen release binary into $1 (default ~/.local/doxygen) and
# adds its bin directory to $GITHUB_PATH when run under GitHub Actions.
set -euo pipefail

version=1.18.0
install_dir=${1:-$HOME/.local/doxygen}
tag=Release_${version//./_}
url=https://github.com/doxygen/doxygen/releases/download/$tag/doxygen-$version.linux.bin.tar.gz

mkdir -p "$install_dir"
curl -fsSL "$url" | tar -xz -C "$install_dir" --strip-components=1
"$install_dir/bin/doxygen" --version
if [[ -n ${GITHUB_PATH:-} ]]; then
    echo "$install_dir/bin" >> "$GITHUB_PATH"
fi
