#!/usr/bin/env bash
# Boot the lock_unit firmware in QEMU (as an ESP32-C3, the closest target QEMU
# supports). No hardware needed. Usually run as `./esp.sh qemu ...`.
#
#   qemu_console.sh                          # interactive console (Ctrl-A X quits)
#   qemu_console.sh "user add Alice" "code add 1 1234" "pin 1234" status
#
# Flash state persists in build-qemu/flash.bin between runs, so a second
# invocation simulates a reboot. Pass --fresh to start from a blank flash.
# QEMU has no Bluetooth controller, so these builds run without the link.
set -euo pipefail

TOOLS="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$TOOLS/../apps/lock_unit"
# shellcheck source=idf_env.sh
. "$TOOLS/idf_env.sh"
idf_env_load
QEMU_BIN="$(ls -d "$HOME"/.espressif/tools/qemu-riscv32/*/qemu/bin 2>/dev/null | tail -1)"
export PATH="$QEMU_BIN:$PATH"

cd "$APP_DIR"
if [[ "${1:-}" == "--fresh" ]]; then
    rm -f build-qemu/flash.bin
    shift
fi
if ! build_log=$(idf.py -B build-qemu -D SDKCONFIG=build-qemu/sdkconfig -D IDF_TARGET=esp32c3 \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu" build 2>&1); then
    echo "$build_log"
    exit 1
fi
if [[ ! -f build-qemu/flash.bin ]]; then
    (cd build-qemu && esptool --chip esp32c3 merge-bin --pad-to-size 4MB -o flash.bin @flash_args >/dev/null)
fi

if [[ $# -eq 0 ]]; then
    echo "QEMU console: type 'help'; Ctrl-A X quits." >&2
    exec qemu-system-riscv32 -nographic -machine esp32c3 \
        -drive file=build-qemu/flash.bin,if=mtd,format=raw -serial mon:stdio
fi

{
    sleep 4
    for c in "$@"; do printf '%s\r\n' "$c"; sleep 1.2; done
    sleep 1
} | timeout 120 qemu-system-riscv32 -nographic -machine esp32c3 \
        -drive file=build-qemu/flash.bin,if=mtd,format=raw -serial stdio -monitor none 2>&1 \
  | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' \
  | grep -vE '^(I \([0-9]+\) (boot|esp_image|cpu_start|heap_init|spi_flash|sleep_gpio|main_task|app_init|efuse_init)|ESP-ROM|Build:|rst:|SPIWP|mode:|load:|entry|\s*$)' || true
