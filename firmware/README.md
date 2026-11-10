# Firmware

| Path | What |
|---|---|
| `components/lock_core` | Pure C11 lock logic (users, PINs, rate limiting, log, state machine). Builds for ESP-IDF and the host. |
| `components/secure_link` | Noise IK/XX handshakes + transport encryption (SHA-256/HMAC here, X25519 and ChaCha20-Poly1305 from Monocypher). Portable. |
| `components/link_proto` | Link frames and messages, plus the BLE UUIDs. The contract with the apps (see `docs/LINK_PROTOCOL.md`). Portable. |
| `components/lock_link` | Lock-side link server: paired peers, handshakes, request dispatch. Portable. |
| `components/keypad_core` | Keypad state machine: key entry, link, pairing, feedback. Portable. |
| `components/monocypher` | Vendored Monocypher 4.0.2. |
| `apps/components/ln_central` | ESP-IDF BLE central for the keypad (scan, connect, GATT setup). |
| `apps/components/platform_esp` | ESP-IDF adapters for lock_core: NVS persistence, HMAC PIN digests, time, RNG. |
| `apps/components/ln_store` | ESP-IDF NVS helpers and device-secret generation (shared by both apps). |
| `apps/lock_unit` | Lock unit firmware (ESP32-C6): lock task, BLE GATT server, console, and the Matter-over-Thread Door Lock endpoint (`main/matter_lock.cpp`). Motor is simulated until the driver lands. |
| `apps/keypad` | Wireless keypad firmware (ESP32-C6 for now, H2 later): BLE central, button matrix, LEDs and buzzer, console. |
| `test/host` | Host tests: lock_core, Noise test vectors, protocol, and end-to-end keypad ↔ lock over an in-memory transport. |
| `../esp.sh` | Run any `idf.py` command without sourcing ESP-IDF: one app, `all` apps, `host` tests, or `qemu`. Also `boards` (list plugged-in boards and their app) and `send` (type console commands). |
| `tools/bench/bench.py` | Two-board bench tests over real BLE: latency, lockout, reboots, persistence. |
| `tools/qemu_console.sh` | Run lock_unit in QEMU, interactive or scripted (`./esp.sh qemu`). |
| `tools/compile_db.py` | Build the merged `compile_commands.json` that clangd and clang-tidy use. |
| `tools/lint.sh` | Strict clang-tidy (every check, findings are errors) over all sources. |
| `tools/clangd`, `tools/clang-tidy` | Wrappers that run Espressif's LLVM (`tools/esp_clang.sh`), so editor and lint match. |

## Toolchain setup (macOS)

ESP-IDF **v6.0.2** is pinned because it is the version esp-matter recommends, and the lock's Matter-over-Thread build needs esp-matter.

```sh
brew install cmake ninja dfu-util libgcrypt glib pixman sdl2 libslirp
mkdir -p ~/esp && cd ~/esp
git clone -b v6.0.2 --depth 1 --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git esp-idf-v6.0.2
cd esp-idf-v6.0.2 && ./install.sh esp32c6,esp32h2,esp32c3
python3 tools/idf_tools.py install qemu-riscv32   # optional: run without hardware
python3 tools/idf_tools.py install esp-clang      # clangd and clang-tidy (editor, tools/lint.sh)
```

Matter needs esp-matter `release/v1.6.1` (the release that recommends IDF v6.0.2) in `~/esp/esp-matter-v1.6.1`:

```sh
cd ~/esp && git clone -b release/v1.6.1 --depth 1 https://github.com/espressif/esp-matter.git esp-matter-v1.6.1
cd esp-matter-v1.6.1 && git submodule update --init --depth 1
cd connectedhomeip/connectedhomeip && ./scripts/checkout_submodules.py --platform esp32 darwin --shallow && cd ../..
. ~/esp/esp-idf-v6.0.2/export.sh && ./install.sh --no-host-tool
```

`--no-host-tool` skips chip-tool, which does not build on Apple Silicon without Rosetta (its zap-cli is x86-only). Apple Home (or another controller) does the commissioning, so chip-tool is not needed. `./esp.sh` loads esp-matter automatically when it is there.

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
| Rules | `firmware/.clang-tidy`: every check on, every finding an error, minus a short documented list of checks that can't apply here. The same rules for all of our code (apps, components, tests). The only other one is `components/monocypher/.clang-tidy`, which turns vendored code off | clangd and `lint.sh` |
| Tool | `tools/esp_clang.sh` picks the LLVM build; `tools/clangd` and `tools/clang-tidy` wrap it | the editor and `lint.sh` |
| Compile flags | `firmware/compile_commands.json` from `tools/compile_db.py` | clangd and `lint.sh` |

- **`.clang-tidy`** turns on every check and treats every finding as an error. The few exclusions are checks written for other projects (Intel FPGA, LLVM's libc, Fuchsia), alias checks that would report the same finding twice, and checks with no call-site fix. Each one is listed with its reason in the file that turns it off. Prefer a fix, or `NOLINT(check): reason`, over another exclusion.
- **`firmware/.clangd`** holds only editor behaviour: which database to use, inlay hints, indexing. It defines no diagnostics. clangd's own include checker is off, because `misc-include-cleaner` in `.clang-tidy` does that job.
- **`compile_db.py`** merges the lock and keypad builds and the host tests into one database. It rewrites ESP-IDF's GCC commands so clang can parse them: RISC-V target, picolibc and libstdc++ headers, GCC-only flags dropped. It also fails if any C/C++ file in the repo is in no build, since such a file would be silently unparsed and unlinted. Downloaded `managed_components` are skipped. `lint.sh` refreshes the database on every run (and configures the builds the first time).

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

## Console (development stand-in for keypad/Matter/app)

```
user add Alice                 code add 1 1234          pin 1234       lock
user add Walker schedule       sched 2 12345 12:00 13:00                time 1800000000 -300
remote unlock [pin]            app unlock               config         config relock 30
sim door open|closed           sim turn locked|unlocked sim jam        status   log
matter [status]                matter commission [3..15] matter reset confirm
matter radio [reset]           matter radio link on|off
factory-reset confirm          # everything, Matter included
```

`remote` runs a lock/unlock as if from Matter (same policy), without a controller.

## Matter over Thread (Apple Home)

The lock itself is the Matter device: a Door Lock endpoint over Thread, no hub. It needs a Thread border router on your network (HomePod mini, Apple TV 4K, Nest Hub, many routers) and esp-matter installed (see Toolchain setup). `apps/lock_unit/sdkconfig.matter` holds the Matter and Thread settings, layered on `sdkconfig.defaults`.

The commissioning window opens at boot for 15 minutes while the lock has no controller. `matter commission` reopens it (also for adding a second controller), and prints the codes. In the Home app: Add Accessory, then scan the QR code (open the `https://project-chip.github.io/...` link it prints) or enter the manual code `3497-011-2332`. Home warns that it is uncertified: it is a development device with test certificates, test vendor/product IDs (0xFFF1/0x8000) and the shared test passcode. Home gives the lock the Thread network's credentials during commissioning.

- Lock and unlock from Home are remote operations: the lock's remote policy (`config remote-unlock`, `config remote-pin`) and operating modes apply.
- Keypad, knob (`sim turn`), console and auto-relock operations show up in Home through the lock task's state updates.
- `matter status` shows the names Home uses (menuconfig: Lochness lock unit, then Matter), the controller count, the window and the Thread state.
- `matter reset confirm` forgets every controller and the Thread network but keeps users, PINs, the log and keypads; `factory-reset confirm` erases all of it.
- Home's auto-lock setting is the lock's own `config relock`: a change on either side shows on the other. The lock does the relocking (it knows the door and the mode); the Door Lock server's own relock timer is kept off.
- A repeated lock or unlock while the bolt is already moving that way succeeds (Home sometimes sends commands twice); the opposite direction is refused as busy.
- `matter radio` shows the Thread channel and the radio's transmit counters (frames, retries, "channel busy" CCA failures); `matter radio reset` zeroes them, and `matter radio link off|on` pauses the keypad link's advertising, for comparing the two.

The lock joins Thread as an end device (a child of one of your border routers), not as a router: as a router, every boot set up links with all neighbor routers at once and spent the first seconds waiting for a free channel (hundreds of failed attempts, all eventually delivered). As an end device the same window shows a steady 10 to 20.

BLE is shared: Matter commissioning advertises on extended advertising set 0 and the keypad link on set 1 (legacy PDUs from the lock's public address, which keypads store when they pair). Both run at once, and BLE stays up after commissioning for the keypad.

Not yet: users and PINs through Matter, Matter time sync (the console's `time` stands in), sleepy end device / Matter ICD (rx stays on for now), OTA, and per-device setup codes.

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

## Which board is which

```sh
./esp.sh boards                     # port, app (lock_unit / keypad), USB serial number
./esp.sh send /dev/cu.usbserial-1120 status   # type console commands, no reset
```

`boards` asks each board's console for `help` without resetting it and names the app from a command only it has. The USB serial number sticks with the board when its port name changes.

## Run without hardware

```sh
./esp.sh qemu                       # interactive lock console; Ctrl-A X quits
./esp.sh qemu --fresh "user add Alice" "code add 1 1234" "pin 1234" status
./esp.sh qemu "user list" status     # second run = reboot; state persists
```

The lock_unit app runs as an ESP32-C3, the closest chip QEMU emulates, built by its own small project in `apps/lock_unit/qemu/` (so its downloaded components don't clash with the Matter build's). Flash is kept in `apps/lock_unit/qemu/build/flash.bin` across runs, whether interactive or scripted, so each run is a reboot. Pass `--fresh` to start from a blank flash.

QEMU has no Bluetooth controller and the C3 has no Thread radio, so the QEMU project adds `apps/lock_unit/sdkconfig.qemu` and leaves Matter out. The link is covered by the host tests.

## Security notes for dev boards

- `CONFIG_LOCHNESS_PIN_KEY_DEV_NVS` (default) keeps the PIN HMAC key in plaintext NVS. Anyone with the flash can brute-force PINs offline. Production uses `CONFIG_LOCHNESS_PIN_KEY_EFUSE` (HMAC peripheral, unreadable eFuse key) together with flash and NVS encryption.
- **Do not burn eFuses on shared dev boards.** It's irreversible. eFuse key provisioning lands with the hardening milestone.
