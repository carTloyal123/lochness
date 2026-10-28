#!/usr/bin/env bash
# Run idf.py without sourcing ESP-IDF first. Arguments pass straight through.
#
#   ./esp.sh lock_unit flash monitor   # first argument may name an app
#   ./esp.sh all build                 # run the command in every app
#   ./esp.sh host                      # build and run the host tests
#   ./esp.sh qemu [--fresh] [cmd...]   # lock_unit in QEMU: interactive, or
#                                      # type the given console commands
#   ./esp.sh -C path/to/project build  # or idf.py's own -C / current directory
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
APPS="$ROOT/firmware/apps"

if [[ "${1:-}" == host ]]; then
    build="$ROOT/firmware/test/host/build"
    cmake -S "$ROOT/firmware/test/host" -B "$build" >/dev/null
    cmake --build "$build" -j
    exec "$build/test_lock_core"
fi

if [[ "${1:-}" == qemu ]]; then
    shift
    exec "$ROOT/firmware/tools/qemu_console.sh" "$@"
fi

# shellcheck source=firmware/tools/idf_env.sh
. "$ROOT/firmware/tools/idf_env.sh"
idf_env_load

if [[ "${1:-}" == all ]]; then
    shift
    for app in "$APPS"/*/; do
        echo "==> $(basename "$app")"
        idf.py -C "$app" "$@"
    done
    exit 0
fi
if [[ $# -gt 0 && -d "$APPS/$1" ]]; then
    app="$1"
    shift
    exec idf.py -C "$APPS/$app" "$@"
fi
exec idf.py "$@"
