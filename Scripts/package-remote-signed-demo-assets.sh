#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

VERSION="${VERSION:-1.0.0}"
RELEASE_TAG="${RELEASE_TAG:-remote-demo-v${VERSION}}"
RELEASE_BASE_URL="${RELEASE_BASE_URL:-https://github.com/Tamber-Inc/eacp/releases/download/${RELEASE_TAG}}"
OUT_DIR="${OUT_DIR:-${REPO_ROOT}/dist/remote-signed-demo}"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build-remote-signed-demo}"
KEYCHAIN_PATH="${HOME}/Library/Keychains/tamber-eacp-remote-demo.keychain-db"
KEYCHAIN_PASSWORD="tamber-eacp-remote-demo-local"

APPHUB_ZIP="AppHub-remote-demo.app.zip"
DEMO_ZIP="TamberLocalUpdateDemo-${VERSION}.app.zip"
DEMO_APP_NAME="Tamber Local Update Demo.app"
DEMO_BINARY_NAME="Tamber Local Update Demo"
PRODUCT_ID="com.tamber.RealUpdateDemo"

log()
{
    printf "\n==> %s\n" "$1"
}

run()
{
    printf "+ %s\n" "$*"
    "$@"
}

sha256_file()
{
    shasum -a 256 "$1" | awk '{print $1}'
}

decode_base64_to_file()
{
    local out="$1"

    if printf "%s" "$APPLE_DEVELOPER_CERTIFICATE_BASE64" | base64 --decode > "$out" 2>/dev/null; then
        return
    fi

    printf "%s" "$APPLE_DEVELOPER_CERTIFICATE_BASE64" | base64 -D > "$out"
}

ensure_keychain_search_list()
{
    local current=()
    local keychain

    while IFS= read -r keychain; do
        current+=("$keychain")
    done < <(security list-keychains -d user | sed 's/^[[:space:]]*"//; s/"$//')

    if printf "%s\n" "${current[@]}" | grep -Fxq "$KEYCHAIN_PATH"; then
        return
    fi

    security list-keychains -d user -s "${current[@]}" "$KEYCHAIN_PATH"
}

has_signing_identity()
{
    security find-identity -v -p codesigning "$KEYCHAIN_PATH" \
        | grep -Fq "\"${APPLE_SIGNING_IDENTITY}\""
}

ensure_tamber_signing_identity()
{
    local temp_dir
    local p12

    if [[ -z "${APPLE_SIGNING_IDENTITY:-}"
          || -z "${APPLE_DEVELOPER_CERTIFICATE_BASE64:-}"
          || -z "${APPLE_DEVELOPER_CERTIFICATE_PASSWORD:-}" ]]; then
        printf "Missing Tamber signing environment. Run under op/ops.env.\n" >&2
        exit 1
    fi

    if [[ ! -f "$KEYCHAIN_PATH" ]]; then
        run security create-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN_PATH"
    fi

    run security unlock-keychain -p "$KEYCHAIN_PASSWORD" "$KEYCHAIN_PATH"
    run security set-keychain-settings -lut 21600 "$KEYCHAIN_PATH"
    ensure_keychain_search_list

    if ! has_signing_identity; then
        temp_dir="$(mktemp -d)"
        p12="${temp_dir}/cert.p12"
        decode_base64_to_file "$p12"
        run security import "$p12" \
            -k "$KEYCHAIN_PATH" \
            -P "$APPLE_DEVELOPER_CERTIFICATE_PASSWORD" \
            -T /usr/bin/codesign \
            -T /usr/bin/security
        rm -rf "$temp_dir"
    fi

    run security set-key-partition-list \
        -S apple-tool:,apple:,codesign: \
        -s \
        -k "$KEYCHAIN_PASSWORD" \
        "$KEYCHAIN_PATH"

    if ! has_signing_identity; then
        printf "Signing identity not found after import: %s\n" "$APPLE_SIGNING_IDENTITY" >&2
        exit 1
    fi
}

sign_path()
{
    local path="$1"

    run codesign --force --timestamp --options runtime \
        --keychain "$KEYCHAIN_PATH" \
        --sign "$APPLE_SIGNING_IDENTITY" \
        "$path"
    run codesign --verify --strict --verbose=2 "$path"
}

cd "$REPO_ROOT"

if [[ "$(uname -s)" != "Darwin" ]]; then
    printf "Remote signed demo packaging must run on macOS.\n" >&2
    exit 1
fi

log "Import Tamber Developer ID signing identity"
ensure_tamber_signing_identity

log "Configure release build"
run cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DEACP_REAL_UPDATE_DEMO_VERSION="$VERSION"

log "Build signed-demo targets"
run cmake --build "$BUILD_DIR" --target AppHub RealUpdateDemo

APPHUB_APP="${BUILD_DIR}/Apps/System/AppHub/AppHub.app"
APPHUB_HELPER="${APPHUB_APP}/Contents/Library/LaunchServices/com.tamber.AppHub.PrivilegedHelper"
DEMO_APP="${BUILD_DIR}/Apps/System/RealUpdateDemo/${DEMO_APP_NAME}"

log "Sign AppHub helper and app"
sign_path "$APPHUB_HELPER"
sign_path "$APPHUB_APP"

log "Sign Demo App"
sign_path "$DEMO_APP"

log "Verify Demo App version"
run "${DEMO_APP}/Contents/MacOS/${DEMO_BINARY_NAME}" --version

log "Package release assets"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"
run ditto -c -k --keepParent "$APPHUB_APP" "${OUT_DIR}/${APPHUB_ZIP}"
run ditto -c -k --keepParent "$DEMO_APP" "${OUT_DIR}/${DEMO_ZIP}"

DEMO_SHA="$(sha256_file "${OUT_DIR}/${DEMO_ZIP}")"
cat > "${OUT_DIR}/manifest.json" <<JSON
{
  "productId": "${PRODUCT_ID}",
  "name": "Tamber Local Update Demo",
  "version": "${VERSION}",
  "bundleName": "${DEMO_APP_NAME}",
  "artifact": {
    "url": "${RELEASE_BASE_URL}/${DEMO_ZIP}",
    "sha256": "${DEMO_SHA}"
  }
}
JSON

log "Release assets"
ls -lh "$OUT_DIR"
cat "${OUT_DIR}/manifest.json"
printf "\n"
