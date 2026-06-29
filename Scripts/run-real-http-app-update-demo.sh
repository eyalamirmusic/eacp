#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

APP_NAME="Tamber Local Update Demo.app"
PRODUCT_ID="com.tamber.RealUpdateDemo"
PORT="${PORT:-8765}"
WORK_ROOT="${WORK_ROOT:-/private/tmp/eacp-real-http-update-demo}"
SERVER_ROOT="${WORK_ROOT}/server"
DOWNLOAD_ROOT="${WORK_ROOT}/downloads"
INSTALL_PATH="/Applications/${APP_NAME}"
ROLLBACK_PATH="/Applications/${APP_NAME}.rollback"
DELAY="${DEMO_DELAY:-1.0}"
KEYCHAIN_PATH="${HOME}/Library/Keychains/tamber-eacp-real-update-demo.keychain-db"
KEYCHAIN_PASSWORD="tamber-eacp-real-update-demo-local"

server_pid=""
downloaded_artifact=""
signing_ready=0

say()
{
    printf "\n%s\n" "$1"
    sleep "$DELAY"
}

run()
{
    printf "\n$ %s\n" "$*"
    "$@"
}

sudo_if_needed()
{
    if [[ -w /Applications ]]; then
        "$@"
    else
        sudo "$@"
    fi
}

cleanup()
{
    if [[ -n "$server_pid" ]]; then
        kill "$server_pid" >/dev/null 2>&1 || true
        wait "$server_pid" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

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

    if [[ "$signing_ready" == "1" ]]; then
        return
    fi

    if [[ -z "${APPLE_SIGNING_IDENTITY:-}"
          || -z "${APPLE_DEVELOPER_CERTIFICATE_BASE64:-}"
          || -z "${APPLE_DEVELOPER_CERTIFICATE_PASSWORD:-}" ]]; then
        printf "Tamber signing requires APPLE_SIGNING_IDENTITY, APPLE_DEVELOPER_CERTIFICATE_BASE64, and APPLE_DEVELOPER_CERTIFICATE_PASSWORD.\n" >&2
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
        printf "Imported signing material, but identity was not found: %s\n" "$APPLE_SIGNING_IDENTITY" >&2
        exit 1
    fi

    signing_ready=1
}

installed_version()
{
    local executable="${INSTALL_PATH}/Contents/MacOS/Tamber Local Update Demo"

    if [[ ! -x "$executable" ]]; then
        printf "not installed\n"
        return
    fi

    "$executable" --version
}

show_installed_state()
{
    printf "\nInstalled app: %s\n" "$INSTALL_PATH"
    printf "Installed version: "
    installed_version

    if [[ -f "${INSTALL_PATH}/Contents/Info.plist" ]]; then
        printf "Bundle version: "
        /usr/libexec/PlistBuddy \
            -c 'Print :CFBundleShortVersionString' \
            "${INSTALL_PATH}/Contents/Info.plist"
    fi
}

sign_app_bundle()
{
    local app="$1"

    if [[ "${USE_TAMBER_SIGNING:-0}" == "1" ]]; then
        ensure_tamber_signing_identity
        run codesign --force --timestamp --options runtime \
            --keychain "$KEYCHAIN_PATH" \
            --sign "$APPLE_SIGNING_IDENTITY" \
            "$app"
    else
        run codesign --force --deep --sign - "$app"
    fi
}

build_version()
{
    local version="$1"
    local build_dir="${WORK_ROOT}/build-${version}"
    local publish_dir="${SERVER_ROOT}/${version}"
    local app_path="${build_dir}/Apps/System/RealUpdateDemo/${APP_NAME}"
    local zip_path="${publish_dir}/TamberLocalUpdateDemo-${version}.app.zip"
    local url_path="${version}/$(basename "$zip_path")"
    local hash

    say "Build ${APP_NAME} ${version} from source."
    run cmake -S "$REPO_ROOT" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DEACP_REAL_UPDATE_DEMO_VERSION="$version"
    run cmake --build "$build_dir" --target RealUpdateDemo

    sign_app_bundle "$app_path"

    mkdir -p "$publish_dir"
    rm -f "$zip_path"
    say "Package ${version} as the update artifact."
    run ditto -c -k --keepParent "$app_path" "$zip_path"

    hash="$(sha256_file "$zip_path")"
    cat > "${SERVER_ROOT}/manifest-${version}.json" <<JSON
{
  "productId": "${PRODUCT_ID}",
  "name": "Tamber Local Update Demo",
  "version": "${version}",
  "artifact": {
    "url": "http://127.0.0.1:${PORT}/${url_path}",
    "sha256": "${hash}"
  }
}
JSON

    cp "${SERVER_ROOT}/manifest-${version}.json" "${SERVER_ROOT}/manifest.json"
    printf "%s\n" "$hash"
}

start_server()
{
    say "Start a local HTTP update server at http://127.0.0.1:${PORT}/."
    (cd "$SERVER_ROOT" && python3 -m http.server "$PORT" --bind 127.0.0.1) \
        > "${WORK_ROOT}/http-server.log" 2>&1 &
    server_pid="$!"
    sleep 1
}

download_current_manifest_and_artifact()
{
    local label="$1"
    local manifest="${DOWNLOAD_ROOT}/${label}-manifest.json"
    local artifact="${DOWNLOAD_ROOT}/${label}.app.zip"
    local expected
    local url
    local actual

    mkdir -p "$DOWNLOAD_ROOT"

    say "Download the current manifest over HTTP."
    run curl --fail --silent --show-error \
        "http://127.0.0.1:${PORT}/manifest.json" \
        --output "$manifest"
    cat "$manifest"
    printf "\n"

    url="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["artifact"]["url"])' "$manifest")"
    expected="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["artifact"]["sha256"])' "$manifest")"

    say "Download the app artifact from the manifest URL."
    run curl --fail --silent --show-error "$url" --output "$artifact"

    actual="$(sha256_file "$artifact")"
    printf "Expected SHA-256: %s\n" "$expected"
    printf "Actual SHA-256:   %s\n" "$actual"
    if [[ "$actual" != "$expected" ]]; then
        printf "Downloaded artifact hash mismatch.\n" >&2
        exit 1
    fi

    downloaded_artifact="$artifact"
}

install_downloaded_artifact()
{
    local artifact="$1"
    local unpack_root="${WORK_ROOT}/unpack"
    local unpacked_app="${unpack_root}/${APP_NAME}"

    rm -rf "$unpack_root"
    mkdir -p "$unpack_root"

    say "Unpack the downloaded artifact."
    run ditto -x -k "$artifact" "$unpack_root"

    if [[ ! -d "$unpacked_app" ]]; then
        printf "Expected unpacked app missing: %s\n" "$unpacked_app" >&2
        exit 1
    fi

    say "Install to ${INSTALL_PATH}, preserving a rollback copy if one exists."
    sudo_if_needed rm -rf "$ROLLBACK_PATH"
    if [[ -d "$INSTALL_PATH" ]]; then
        sudo_if_needed mv "$INSTALL_PATH" "$ROLLBACK_PATH"
    fi
    sudo_if_needed ditto "$unpacked_app" "$INSTALL_PATH"
}

if [[ "$(uname -s)" != "Darwin" ]]; then
    printf "This demo installs a macOS .app bundle and must run on macOS.\n" >&2
    exit 1
fi

if [[ "${USE_TAMBER_SIGNING:-0}" == "1"
      && -z "${APPLE_SIGNING_IDENTITY:-}"
      && -z "${EACP_REAL_UPDATE_OP_WRAPPED:-}" ]]; then
    export EACP_REAL_UPDATE_OP_WRAPPED=1
    exec op run \
        --env-file=/Users/jamiepond/projects/TamberWorkspace/tamber-web/op/ops.env \
        -- "$0" "$@"
fi

say "Prepare a clean local update workspace."
rm -rf "$WORK_ROOT"
mkdir -p "$SERVER_ROOT" "$DOWNLOAD_ROOT"

start_server

build_version "1.0.0"
download_current_manifest_and_artifact v1
install_downloaded_artifact "$downloaded_artifact"
show_installed_state

say "Now publish a real update: rebuild the app as version 2.0.0 and update the manifest."
build_version "2.0.0"
run curl --fail --silent --show-error \
    "http://127.0.0.1:${PORT}/manifest.json" \
    --output "${DOWNLOAD_ROOT}/published-v2-manifest.json"
cat "${DOWNLOAD_ROOT}/published-v2-manifest.json"
printf "\n"

download_current_manifest_and_artifact v2
install_downloaded_artifact "$downloaded_artifact"
show_installed_state

say "Done. The installed /Applications app was downloaded over local HTTP and updated from 1.0.0 to 2.0.0."
