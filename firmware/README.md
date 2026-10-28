# Firmware

| Path | What |
|---|---|
| `components/lock_core` | Pure C11 lock logic (users, PINs, rate limiting, log, state machine). Builds for ESP-IDF and the host. |
| `components/platform_esp` | ESP-IDF adapters for lock_core: NVS persistence, HMAC PIN digests, time, RNG. |
| `apps/lock_unit` | Lock unit firmware (ESP32-C6). Motor is simulated until the driver lands. |
| `test/host` | Host unit tests for lock_core. |
| `tools/qemu_console.sh` | Run lock_unit in QEMU and script its console. |

## Toolchain setup (macOS)

ESP-IDF **v6.0.2** is pinned because it is the version esp-matter recommends, and the hub will need esp-matter.

```sh
brew install cmake ninja dfu-util libgcrypt glib pixman sdl2 libslirp
mkdir -p ~/esp && cd ~/esp
git clone -b v6.0.2 --depth 1 --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git esp-idf-v6.0.2
cd esp-idf-v6.0.2 && ./install.sh esp32c6,esp32h2,esp32c3
python3 tools/idf_tools.py install qemu-riscv32   # optional: run without hardware
```

Each new shell: `. ~/esp/esp-idf-v6.0.2/export.sh` (an alias like `alias idf='. ~/esp/esp-idf-v6.0.2/export.sh'` helps).

## Build, flash and monitor (ESP32-C6-DevKitC-1)

```sh
cd firmware/apps/lock_unit
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor   # use the board's "UART" USB port
```

`idf.py menuconfig` → **Lochness lock unit** sets the door-sensor GPIO and simulated motor timing. **Lochness platform** selects the PIN key source.

## Console (development stand-in for keypad/hub/app)

```
user add Alice                 code add 1 1234          pin 1234       lock
user add Walker schedule       sched 2 12345 12:00 13:00                time 1800000000 -300
remote unlock [pin]            app unlock               config         config relock 30
sim door open|closed           sim turn locked|unlocked sim jam        status   log
factory-reset confirm
```

## Run without hardware

```sh
firmware/tools/qemu_console.sh --fresh "user add Alice" "code add 1 1234" "pin 1234" status
firmware/tools/qemu_console.sh "user list" status      # second run = reboot; state persists
```

## Security notes for dev boards

- `CONFIG_LOCHNESS_PIN_KEY_DEV_NVS` (default) keeps the PIN HMAC key in plaintext NVS. Anyone with the flash can brute-force PINs offline. Production uses `CONFIG_LOCHNESS_PIN_KEY_EFUSE` (HMAC peripheral, unreadable eFuse key) together with flash and NVS encryption.
- **Do not burn eFuses on shared dev boards.** It's irreversible. eFuse key provisioning lands with the hardening milestone.
