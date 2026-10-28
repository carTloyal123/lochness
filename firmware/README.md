# Firmware

| Path | What |
|---|---|
| `components/lock_core` | Pure C11 lock logic (users, PINs, rate limiting, log, state machine). Builds for ESP-IDF and the host. |
| `components/secure_link` | Noise IK/XX handshakes + transport encryption (SHA-256/HMAC here, X25519 and ChaCha20-Poly1305 from Monocypher). Portable. |
| `components/link_proto` | Link frames and messages, plus the BLE UUIDs. The contract with the apps (see `docs/LINK_PROTOCOL.md`). Portable. |
| `components/lock_link` | Lock-side link server: paired peers, handshakes, request dispatch. Portable. |
| `components/keypad_core` | Keypad state machine: key entry, link, pairing, feedback. Portable. |
| `components/monocypher` | Vendored Monocypher 4.0.2. |
| `components/platform_esp` | ESP-IDF adapters for lock_core: NVS persistence, HMAC PIN digests, time, RNG. |
| `components/ln_store` | ESP-IDF NVS helpers and device-secret generation (shared by both apps). |
| `apps/lock_unit` | Lock unit firmware (ESP32-C6): lock task, BLE GATT server, console. Motor is simulated until the driver lands. |
| `apps/keypad` | Wireless keypad firmware (ESP32-C6 for now, H2 later): BLE central, button matrix, LEDs and buzzer, console. |
| `test/host` | Host tests: lock_core, Noise test vectors, protocol, and end-to-end keypad ↔ lock over an in-memory transport. |
| `tools/qemu_console.sh` | Run lock_unit in QEMU and script its console. |
| `tools/compile_db.py` | Build the merged `compile_commands.json` that clangd and clang-tidy use. |
| `tools/lint.sh` | Run clang-tidy with every check (`.clang-tidy`). |

## Toolchain setup (macOS)

ESP-IDF **v6.0.2** is pinned because it is the version esp-matter recommends, and the hub will need esp-matter.

```sh
brew install cmake ninja dfu-util libgcrypt glib pixman sdl2 libslirp
mkdir -p ~/esp && cd ~/esp
git clone -b v6.0.2 --depth 1 --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git esp-idf-v6.0.2
cd esp-idf-v6.0.2 && ./install.sh esp32c6,esp32h2,esp32c3
python3 tools/idf_tools.py install qemu-riscv32   # optional: run without hardware
python3 tools/idf_tools.py install esp-clang      # optional: clang-tidy for tools/lint.sh
```

Each new shell: `. ~/esp/esp-idf-v6.0.2/export.sh` (an alias like `alias idf='. ~/esp/esp-idf-v6.0.2/export.sh'` helps).

## Editor (clangd) and lint

```sh
firmware/tools/compile_db.py --configure   # once, and again after adding source files
firmware/tools/lint.sh                     # all checks; or: lint.sh lock_link, lint.sh --fix kp.c
```

`compile_db.py` merges the lock, keypad and host-test builds into `firmware/compile_commands.json`. It also rewrites ESP-IDF's GCC commands so clang can parse them: RISC-V target, picolibc headers, GCC-only flags dropped. `firmware/.clangd` points clangd at that file, so headers resolve in every app and component. The build's own databases under `apps/*/build` are not used.

Any recent clangd works, including Xcode's `/usr/bin/clangd`. Espressif's (`~/.espressif/tools/esp-clang/*/esp-clang/bin/clangd`) matches the clang-tidy that `lint.sh` uses. clangd runs the `.clang-tidy` checks as you type.

In VS Code or Cursor, use the clangd extension and disable Microsoft's C/C++ IntelliSense.

`.clang-tidy` enables every check except a few that target other projects; each is listed with its reason. Monocypher is vendored, so it is not linted.

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

## Pairing a keypad (two dev boards)

Flash `apps/lock_unit` to one C6 board and `apps/keypad` to another, with a serial monitor on each.

```
lochness> user add Alice
lochness> code add 1 1234
lochness> pair keypad              # stand-in for the lock's pair button (60 s window)
keypad>   pair                     # or hold the BOOT button for 3 s
          ... keypad logs "PAIRED"; `peers` on the lock shows it
keypad>   keys 1234#               # → lock unlocks (simulated motor), keypad shows UNLOCKED
keypad>   keys L                   # one-touch lock
```

With no input hardware configured, the keypad's `keys` command stands in for the buttons. `idf.py menuconfig` → **Lochness keypad** sets the button matrix, lock/pair buttons, LEDs and buzzer pins.

## Run without hardware

```sh
firmware/tools/qemu_console.sh --fresh "user add Alice" "code add 1 1234" "pin 1234" status
firmware/tools/qemu_console.sh "user list" status      # second run = reboot; state persists
```

QEMU has no Bluetooth controller, so QEMU builds use `apps/lock_unit/sdkconfig.qemu` to compile the link out. The link is covered by the host tests.

## Security notes for dev boards

- `CONFIG_LOCHNESS_PIN_KEY_DEV_NVS` (default) keeps the PIN HMAC key in plaintext NVS. Anyone with the flash can brute-force PINs offline. Production uses `CONFIG_LOCHNESS_PIN_KEY_EFUSE` (HMAC peripheral, unreadable eFuse key) together with flash and NVS encryption.
- **Do not burn eFuses on shared dev boards.** It's irreversible. eFuse key provisioning lands with the hardening milestone.
