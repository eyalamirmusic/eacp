#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build}"
APPHUB_DIR="${BUILD_DIR}/Apps/System/AppHub"
APP_BUNDLE="${APPHUB_DIR}/AppHub.app"
HELPER_LABEL="${EACP_APPHUB_HELPER_LABEL:-com.tamber.AppHub.PrivilegedHelper}"
HELPER_IN_BUNDLE="${APP_BUNDLE}/Contents/Library/LaunchServices/${HELPER_LABEL}"
APP_REQUIREMENT="${EACP_APPHUB_APP_SIGNING_REQUIREMENT:-identifier \"com.tamber.AppHub\" and anchor apple generic}"
HELPER_REQUIREMENT="${EACP_APPHUB_HELPER_SIGNING_REQUIREMENT:-identifier \"${HELPER_LABEL}\" and anchor apple generic}"
SIGNING_IDENTITY="${EACP_APPHUB_SIGNING_IDENTITY:-}"

log()
{
    printf "\n==> %s\n" "$1"
}

run()
{
    printf "+ %s\n" "$*"
    "$@"
}

find_tamber_identity()
{
    security find-identity -v -p codesigning \
        | sed -n 's/^[[:space:]]*[0-9]*) [A-F0-9]\{40\} "\(Developer ID Application: .*Tamber.*\)".*$/\1/p' \
        | head -n 1
}

if [[ -z "$SIGNING_IDENTITY" ]]; then
    SIGNING_IDENTITY="$(find_tamber_identity)"
fi

if [[ -z "$SIGNING_IDENTITY" ]]; then
    cat >&2 <<'EOF'
No Tamber Developer ID Application signing identity was found in this keychain.

Install/import the Tamber Developer ID Application certificate and private key,
or pass the exact identity explicitly:

  EACP_APPHUB_SIGNING_IDENTITY="Developer ID Application: Tamber ..." Scripts/sign-apphub-jobbless.sh

Available code signing identities:
EOF
    security find-identity -v -p codesigning >&2 || true
    exit 1
fi

cd "$REPO_ROOT"

log "Build AppHub and the embedded privileged helper"
run cmake --build "$BUILD_DIR" --target AppHub

if [[ ! -d "$APP_BUNDLE" ]]; then
    printf "Expected app bundle was not built: %s\n" "$APP_BUNDLE" >&2
    exit 1
fi

if [[ ! -x "$HELPER_IN_BUNDLE" ]]; then
    printf "Expected embedded helper was not built: %s\n" "$HELPER_IN_BUNDLE" >&2
    exit 1
fi

log "Verify helper contains embedded JobBless plists"
run otool -l "$HELPER_IN_BUNDLE"
if ! otool -l "$HELPER_IN_BUNDLE" | grep -q "__info_plist"; then
    printf "Helper is missing embedded __TEXT,__info_plist section.\n" >&2
    exit 1
fi
if ! otool -l "$HELPER_IN_BUNDLE" | grep -q "__launchd_plist"; then
    printf "Helper is missing embedded __TEXT,__launchd_plist section.\n" >&2
    exit 1
fi

log "Sign embedded privileged helper"
run codesign --force --timestamp --options runtime \
    --sign "$SIGNING_IDENTITY" \
    "$HELPER_IN_BUNDLE"

log "Sign AppHub bundle"
run codesign --force --timestamp --options runtime \
    --sign "$SIGNING_IDENTITY" \
    "$APP_BUNDLE"

log "Verify signatures and designated requirements"
run codesign --verify --strict --verbose=4 "$HELPER_IN_BUNDLE"
run codesign --verify --strict --verbose=4 "$APP_BUNDLE"
run codesign --verify --strict --verbose=4 --requirements "=${HELPER_REQUIREMENT}" "$HELPER_IN_BUNDLE"
run codesign --verify --strict --verbose=4 --requirements "=${APP_REQUIREMENT}" "$APP_BUNDLE"

log "Show signed requirements"
run codesign -d --requirements - "$HELPER_IN_BUNDLE"
run codesign -d --requirements - "$APP_BUNDLE"

log "Signed AppHub JobBless artifacts"
printf "App:    %s\n" "$APP_BUNDLE"
printf "Helper: %s\n" "$HELPER_IN_BUNDLE"
printf "Identity: %s\n" "$SIGNING_IDENTITY"
