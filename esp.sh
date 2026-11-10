#!/usr/bin/env bash
# Run idf.py without sourcing ESP-IDF first. Arguments pass straight through.
#
#   ./esp.sh lock_unit flash monitor   # first argument may name an app
#   ./esp.sh all build                 # run the command in every app
#   ./esp.sh host                      # build and run the host tests
#   ./esp.sh qemu [--fresh] [cmd...]   # lock_unit in QEMU: interactive, or
#                                      # type the given console commands
#   ./esp.sh send PORT [--wait S] CMD...  # type console commands into a
#                                      # running board (no reset), e.g.
#                                      # ./esp.sh send /dev/cu.usbserial-1120 "status"
#   ./esp.sh boards [--no-probe]       # list plugged-in boards and their app
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

if [[ "${1:-}" == boards ]]; then
    shift
    # shellcheck source=firmware/tools/idf_env.sh
    . "$ROOT/firmware/tools/idf_env.sh"
    idf_env_load >/dev/null 2>&1 # for pyserial
    exec python3 "$ROOT/firmware/tools/boards.py" "$@"
fi

if [[ "${1:-}" == send ]]; then
    shift
    # shellcheck source=firmware/tools/idf_env.sh
    . "$ROOT/firmware/tools/idf_env.sh"
    idf_env_load >/dev/null 2>&1 # for pyserial
    exec python3 "$ROOT/firmware/tools/send.py" "$@"
fi

# shellcheck source=firmware/tools/idf_env.sh
. "$ROOT/firmware/tools/idf_env.sh"
idf_env_load

if [[ "${1:-}" == all ]]; then
    shift
    for app in "$APPS"/*/; do
        # apps/components holds shared ESP-only components, not an app.
        [[ -f "$app/CMakeLists.txt" ]] || continue
        echo "==> $(basename "$app")"
        idf.py -C "$app" "$@"
    done
    exit 0
fi
if [[ $# -gt 0 && -f "$APPS/$1/CMakeLists.txt" ]]; then
    app="$1"
    shift
    exec idf.py -C "$APPS/$app" "$@"
fi
exec idf.py "$@"
