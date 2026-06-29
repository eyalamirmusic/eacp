#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
DEMO_ROOT="${1:-/private/tmp/eacp-apphub-story-demo}"
DEMO_DELAY="${DEMO_DELAY:-1.5}"

APPHUB=""

say()
{
    printf "\n%s\n" "$1"
    sleep "$DEMO_DELAY"
}

run()
{
    printf "\n$ %s\n" "$*"
    "$@"
    sleep "$DEMO_DELAY"
}

payload()
{
    local product_id="$1"
    local path="${DEMO_ROOT}/Applications/${product_id}/artifact.bin"

    printf "%s payload: " "$product_id"
    if [[ -f "$path" ]]; then
        cat "$path"
        printf "\n"
    else
        printf "not installed\n"
    fi
}

receipt()
{
    local product_id="$1"
    local path="${DEMO_ROOT}/receipts/${product_id}.json"

    printf "\n%s receipt:\n" "$product_id"
    if [[ -f "$path" ]]; then
        cat "$path"
        printf "\n"
    else
        printf "not installed\n"
    fi
}

show_proof()
{
    printf "\nInstalled payload proof:\n"
    payload shared.onnxruntime
    payload shared.clap
    payload tamber.editor
    payload tamber.capture

    receipt shared.clap
    receipt tamber.editor
}

resolve_apphub()
{
    local bundle_bin="${REPO_ROOT}/build/Apps/System/AppHub/AppHub.app/Contents/MacOS/AppHub"
    local plain_bin="${REPO_ROOT}/build/Apps/System/AppHub/AppHub"

    if [[ -x "$bundle_bin" ]]; then
        APPHUB="$bundle_bin"
        return
    fi

    if [[ -x "$plain_bin" ]]; then
        APPHUB="$plain_bin"
        return
    fi

    printf "Could not find built AppHub executable.\n" >&2
    exit 1
}

cd "$REPO_ROOT"

say "Build the AppHub demo binary."
run cmake --build build --target AppHub
resolve_apphub

say "The macOS production hook is now present too: AppHub embeds a JobBless helper in its app bundle."
say "This script does not run bless-helper by default because real blessing requires signing and an admin authorization prompt."
if [[ "${RUN_BLESS:-0}" == "1" ]]; then
    run "$APPHUB" bless-helper
fi

say "User opens Tamber App Hub for the first time. There is a catalog, but nothing is installed yet."
run "$APPHUB" --root "$DEMO_ROOT" reset
run "$APPHUB" --root "$DEMO_ROOT" list

say "User chooses to install Example Editor."
say "The hub plans the app plus its shared resources: ONNX Runtime and the CLAP model."
run "$APPHUB" --root "$DEMO_ROOT" install tamber.editor
run "$APPHUB" --root "$DEMO_ROOT" list
show_proof
sleep "$DEMO_DELAY"

say "User also installs Example Capture from the same hub."
say "The shared resources are already present, so both apps can share them instead of duplicating blobs."
run "$APPHUB" --root "$DEMO_ROOT" install tamber.capture
run "$APPHUB" --root "$DEMO_ROOT" list
show_proof
sleep "$DEMO_DELAY"

say "User launches the installed apps."
run "$APPHUB" --root "$DEMO_ROOT" open tamber.editor
run "$APPHUB" --root "$DEMO_ROOT" open tamber.capture
run "$APPHUB" --root "$DEMO_ROOT" list

say "A new update is published while the user is working."
say "The feed now has Example Editor v2 and CLAP Model v2."
run "$APPHUB" --root "$DEMO_ROOT" publish-update
run "$APPHUB" --root "$DEMO_ROOT" list

say "The hub checks for updates while the apps are still running."
say "Correct behavior: it sees the update, but waits instead of replacing files under a running app."
run "$APPHUB" --root "$DEMO_ROOT" update
show_proof
sleep "$DEMO_DELAY"

say "The user closes the apps."
run "$APPHUB" --root "$DEMO_ROOT" close tamber.capture
run "$APPHUB" --root "$DEMO_ROOT" close tamber.editor
run "$APPHUB" --root "$DEMO_ROOT" list

say "The hub applies the pending update through the mocked privileged helper."
run "$APPHUB" --root "$DEMO_ROOT" update

say "Now the user-visible state is correct: Editor and the shared CLAP model are updated, Capture remains installed at v1."
run "$APPHUB" --root "$DEMO_ROOT" list
show_proof

say "The story is complete. All mocked protected writes stayed under ${DEMO_ROOT}."
