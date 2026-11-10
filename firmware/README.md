# Firmware

| Path | What |
|---|---|
| `components/lock_core` | Pure C11 lock logic (users, PINs, rate limiting, log, state machine). Builds for ESP-IDF and the host. |
| `components/secure_link` | Noise IK/XX handshakes + transport encryption (SHA-256/HMAC here, X25519 and ChaCha20-Poly1305 from Monocypher). Portable. |
| `components/link_proto` | Link frames and messages, plus the BLE UUIDs. The contract with the apps (see `docs/LINK_PROTOCOL.md`). Portable. |
| `components/lock_link` | Lock-side link server: paired peers, handshakes, request dispatch. Portable. |
| `components/keypad_core` | Keypad state machine: key entry, link, pairing, feedback. Portable. |
| `components/hub_core` | Hub side of the link: pairing, a persistent session with reconnect backoff, remote lock/unlock, clock and keepalive, LOCK_STATE updates. Portable. |
| `components/ln_central` | ESP-IDF BLE central shared by the keypad and hub (scan, connect, GATT setup). On the hub it attaches to Matter's NimBLE host. |
| `components/monocypher` | Vendored Monocypher 4.0.2. |
| `components/platform_esp` | ESP-IDF adapters for lock_core: NVS persistence, HMAC PIN digests, time, RNG. |
| `components/ln_store` | ESP-IDF NVS helpers and device-secret generation (shared by both apps). |
| `apps/lock_unit` | Lock unit firmware (ESP32-C6): lock task, BLE GATT server, console. Motor is simulated until the driver lands. |
| `apps/hub` | Hub firmware (ESP32-C6): Matter Door Lock over Wi-Fi (esp-matter), linked to the lock over BLE. |
| `apps/keypad` | Wireless keypad firmware (ESP32-C6 for now, H2 later): BLE central, button matrix, LEDs and buzzer, console. |
| `test/host` | Host tests: lock_core, Noise test vectors, protocol, and end-to-end keypad ↔ lock over an in-memory transport. |
| `../esp.sh` | Run any `idf.py` command without sourcing ESP-IDF: one app, `all` apps, `host` tests, or `qemu`. |
| `tools/bench/bench.py` | Two-board bench tests over real BLE: latency, lockout, reboots, persistence. |
| `tools/qemu_console.sh` | Run lock_unit in QEMU, interactive or scripted (`./esp.sh qemu`). |
| `tools/compile_db.py` | Build the merged `compile_commands.json` that clangd and clang-tidy use. |
| `tools/lint.sh` | Strict clang-tidy (every check, findings are errors) over all sources. |
| `tools/clangd`, `tools/clang-tidy` | Wrappers that run Espressif's LLVM (`tools/esp_clang.sh`), so editor and lint match. |

## Toolchain setup (macOS)

ESP-IDF **v6.0.2** is pinned because it is the version esp-matter recommends, and the hub will need esp-matter.

```sh
brew install cmake ninja dfu-util libgcrypt glib pixman sdl2 libslirp
mkdir -p ~/esp && cd ~/esp
git clone -b v6.0.2 --depth 1 --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git esp-idf-v6.0.2
cd esp-idf-v6.0.2 && ./install.sh esp32c6,esp32h2,esp32c3
python3 tools/idf_tools.py install qemu-riscv32   # optional: run without hardware
python3 tools/idf_tools.py install esp-clang      # clangd and clang-tidy (editor, tools/lint.sh)
```

The hub also needs esp-matter `release/v1.6.1` (the release that recommends IDF v6.0.2) in `~/esp/esp-matter-v1.6.1`:

```sh
cd ~/esp && git clone -b release/v1.6.1 --depth 1 https://github.com/espressif/esp-matter.git esp-matter-v1.6.1
cd esp-matter-v1.6.1 && git submodule update --init --depth 1
cd connectedhomeip/connectedhomeip && ./scripts/checkout_submodules.py --platform esp32 darwin --shallow && cd ../..
. ~/esp/esp-idf-v6.0.2/export.sh && ./install.sh --no-host-tool
```

`--no-host-tool` skips chip-tool, which does not build on Apple Silicon without Rosetta (its zap-cli is x86-only). Apple Home commissions the hub, so chip-tool is not needed. `./esp.sh` loads esp-matter automatically when it is there.

Each new shell: `. ~/esp/esp-idf-v6.0.2/export.sh` (an alias like `alias idf='. ~/esp/esp-idf-v6.0.2/export.sh'` helps).

## Editor (clangd) and lint

```sh
python3 $IDF_PATH/tools/idf_tools.py install esp-clang   # once: Espressif's LLVM
firmware/tools/lint.sh                     # strict: every finding is an error, exit 1
firmware/tools/lint.sh lock_link           # only files matching a regex
firmware/tools/lint.sh --fix kp.c          # apply the suggested fixes
```

The editor and the linter cannot disagree, because each input has exactly one source:

| What | Single source | Used by |
|---|---|---|
| Rules | `.clang-tidy` files: `firmware/.clang-tidy`, plus nested ones that narrow it for a directory (`apps/` for ESP-IDF/Matter glue, `apps/hub/`, `components/ln_store/`, `components/platform_esp/`, `components/ln_central/`; `components/monocypher/` turns everything off) | clangd and `lint.sh` |
| Tool | `tools/esp_clang.sh` picks the LLVM build; `tools/clangd` and `tools/clang-tidy` wrap it | the editor and `lint.sh` |
| Compile flags | `firmware/compile_commands.json` from `tools/compile_db.py` | clangd and `lint.sh` |

- **`.clang-tidy`** turns on every check and treats every finding as an error. The few exclusions are checks written for other projects (Intel FPGA, LLVM's libc, Fuchsia), alias checks that would report the same finding twice, and checks with no call-site fix. Each one is listed with its reason in the file that turns it off. Prefer a fix, or `NOLINT(check): reason`, over another exclusion.
- **`firmware/.clangd`** holds only editor behaviour: which database to use, inlay hints, indexing. It defines no diagnostics. clangd's own include checker is off, because `misc-include-cleaner` in `.clang-tidy` does that job.
- **`compile_db.py`** merges the lock, keypad and hub builds and the host tests into one database. It rewrites ESP-IDF's GCC commands so clang can parse them: RISC-V target, picolibc and libstdc++ headers, GCC-only flags dropped. It also fails if any C/C++ file in the repo is in no build, since such a file would be silently unparsed and unlinted. Downloaded `managed_components` are skipped. `lint.sh` refreshes the database on every run (and configures the builds the first time).

**VS Code / Cursor:** open the repo root and install the recommended clangd extension. `.vscode/settings.json` already points it at `firmware/tools/clangd` and turns off Microsoft's IntelliSense. **Other editors:** set the clangd binary to `firmware/tools/clangd`, not `/usr/bin/clangd` (Xcode's, an older LLVM with different checks).

## Build, flash and monitor (ESP32-C6-DevKitC-1)

From any shell, without sourcing ESP-IDF first:

```sh
./esp.sh all build                 # build every app
./esp.sh lock_unit flash monitor   # any idf.py command in one app
./esp.sh keypad menuconfig
./esp.sh host                      # build and run the host tests
```

`esp.sh` loads ESP-IDF and passes its arguments to `idf.py`. If the first argument names an app, it runs in that app, and `all` runs it in each app in turn. Otherwise it runs in the current directory. Use the board's "UART" USB port; add `-p /dev/cu.usbserial-XXXX` if more than one is connected.

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

## Status LED

The lock shows its bolt on the DevKitC-1's RGB LED (GPIO 8): red locked, green unlocked, amber moving, blinking red jammed, blinking blue while a pairing window is open. `idf.py menuconfig` → **Lochness lock unit** sets the pin (-1 turns it off) and brightness.

## Bench tests (two boards)

```sh
. firmware/tools/idf_env.sh && idf_env_load    # for pyserial; or pip install pyserial
python3 firmware/tools/bench/bench.py --lock /dev/cu.usbserial-210 --keypad /dev/cu.usbserial-110
```

It factory-resets the lock, re-pairs the keypad, then measures cold, typed and warm request latency and checks lockout (including across a lock reboot), recovery from either board rebooting mid-session, persistence across power cycles, battery and log reporting, and that pairing without a window fails. `--keep` reuses the current pairing, `--only a,b` picks tests, `--json` saves results. The keypad logs `timing: link … ms, result … ms after connect` for each cold request.

## Hub and Apple Home

Flash `apps/hub` to a third C6. Its console is the Matter shell: hub commands are `matter esp hub status|commission [min]|pair|forget confirm|lock [pin]|unlock [pin]`. `./esp.sh send PORT CMD...` types commands into a running board from the Mac without resetting it.

```
lochness> pair hub                 # lock: open a 60 s window for the hub role
> matter esp hub pair              # hub: logs "pairing PAIRED", then "link to lock UP"
```

Then add it in the Home app: Add Accessory → scan the QR code (`matter onboardingcodes ble` on the hub prints it) or enter the manual code `3497-011-2332`. It is a development device with test certificates, so Home warns that it is uncertified. The iPhone gives the hub your 2.4 GHz Wi-Fi network. The commissioning window closes 15 minutes after boot. To reopen it without a reset: `./esp.sh send <hub port> "matter esp hub commission"` (or `matter esp hub commission [3..15 min]` on the console). The same command adds a second controller later.

Home names the accessory from Basic Information: vendor `CONFIG_HUB_VENDOR_NAME` ("Lochness"), product `CONFIG_HUB_PRODUCT_NAME` ("Lochness Hub"), serial `LN-` + Wi-Fi MAC, software version from `PROJECT_VER` (see `main/device_info.cpp`; `matter esp hub status` prints them). While commissioning it advertises over BLE as `Lochness-<discriminator>`. Vendor and product IDs stay the Matter test IDs (0xFFF1/0x8000) until there is a real VID.

- Lock and unlock from Home go to the lock, which applies its own remote policy (`config remote-unlock`, `config remote-pin`).
- Keypad, knob (`sim turn`) and auto-relock operations show up in Home through the lock's state pushes.
- The hub gives the lock the time from SNTP (`CONFIG_HUB_SNTP_SERVER`; point it at your router, or leave it empty to disable). Set your time zone in `CONFIG_HUB_TZ` (POSIX TZ string, default `UTC0`) so weekday schedules use local time.

## Run without hardware

```sh
./esp.sh qemu                       # interactive lock console; Ctrl-A X quits
./esp.sh qemu --fresh "user add Alice" "code add 1 1234" "pin 1234" status
./esp.sh qemu "user list" status     # second run = reboot; state persists
```

The lock_unit app runs as an ESP32-C3, the closest chip QEMU emulates. Flash is kept in `apps/lock_unit/build-qemu/flash.bin` across runs, whether interactive or scripted, so each run is a reboot. Pass `--fresh` to start from a blank flash.

QEMU has no Bluetooth controller, so QEMU builds use `apps/lock_unit/sdkconfig.qemu` to compile the link out. The link is covered by the host tests.

## Security notes for dev boards

- `CONFIG_LOCHNESS_PIN_KEY_DEV_NVS` (default) keeps the PIN HMAC key in plaintext NVS. Anyone with the flash can brute-force PINs offline. Production uses `CONFIG_LOCHNESS_PIN_KEY_EFUSE` (HMAC peripheral, unreadable eFuse key) together with flash and NVS encryption.
- **Do not burn eFuses on shared dev boards.** It's irreversible. eFuse key provisioning lands with the hardening milestone.
