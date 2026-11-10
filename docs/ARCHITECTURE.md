# Lochness: Architecture (Draft v1)

A local-first smart lock. Two principles drive the design, in this order:

1. **Maximum user privacy.** No cloud, no accounts, no telemetry. The lock is fully functional with zero internet access.
2. **Ease of use.** Normal use happens through the user's existing Matter ecosystem (Apple Home, Google Home, Home Assistant, SmartThings, …). A companion app exists only for things Matter cannot do.

### Decisions so far

| Decision | Choice |
|---|---|
| Topology | **The lock is the Matter device** (Matter over Thread). No hub: it joins the user's Thread network through an existing border router (HomePod, Apple TV, Nest Hub, …). The keypad talks to the lock over BLE. |
| Keypad | **Always wireless**, battery powered, mounted outside |
| Lock form factor | **Retrofit**: replaces the interior thumb-turn. The exterior cylinder and key stay unchanged. **US single-cylinder deadbolts first** (Kwikset, Schlage, Yale and similar tailpieces). |
| Keypad input | **Mechanical buttons and capacitive touch**, behind one input-driver interface in the keypad firmware |
| Toolchain | **ESP-IDF v6.0.2** (pinned to esp-matter's recommended version). PlatformIO was rejected because esp-matter doesn't support it. |
| Companion app | **Native**: Swift (iOS) first, Kotlin (Android) later |

---

## 1. System topology

```
   EXTERIOR (lock face + key unchanged) │  INTERIOR
                                        │
  ┌───────────────────────┐   BLE       │  ┌──────────────────────────────┐  Thread   ┌──────────────────────┐   IP    ┌────────────────────┐
  │ Wireless keypad       │  secure     │  │ Lock unit (battery)          │◄────────►│ Thread border router │◄──────►│ Apple/Google/HA/…  │
  │ - battery, deep sleep │  channel    │  │ replaces interior thumb-turn │  Matter  │ (user's HomePod,     │ Matter │ controllers        │
  │ - sends PIN attempts  ├─────────────┼─►│ - Matter Door Lock endpoint  │          │  Apple TV, Nest, …)  │        └────────────────────┘
  │ - holds NO PINs       │             │  │ - credential DB (truth)      │          └──────────────────────┘
  │ - tamper switch       │             │  │ - every access decision      │
  └───────────────────────┘             │  │ - motor, encoder, door sensor│
                                        │  └──────────────▲───────────────┘
                                        │                 │ BLE (setup / advanced only)
                                        │          ┌──────┴──────┐
                                        │          │  iOS app    │
                                        │          └─────────────┘
```

The keypad talks **directly to the lock unit**. Keypad entry keeps working when Thread, Wi-Fi or the border router is down.

### Responsibilities

| Unit | Role | Trust level |
|---|---|---|
| **Keypad** | Captures key presses and sends PIN attempts to the lock over an encrypted channel. Drives LEDs and the buzzer from the lock's reply. Holds no PINs and cannot unlock without a correct PIN. Outside the door, so assume attackers can steal or replace it. | Untrusted input device |
| **Lock unit** | Security root and Matter node. Stores users, credentials and schedules. Makes every lock/unlock decision, drives the motor, enforces rate limits and keeps the access log. Serves the Matter Door Lock cluster over Thread. Works fully standalone. | Root of trust |
| **Matter controllers** | Lock/unlock, state, users and schedules through the user's ecosystem. Remote commands go through the same lock-side policy as everything else. | Trusted, but capped by lock-side policy |
| **App** | Setup, calibration, diagnostics and recovery, over BLE straight to the lock unit. | Per-phone keys. Enrollment requires physical access. |

### Why the lock is the Matter node

An earlier prototype put Matter on a mains-powered Wi-Fi hub that talked to the lock over BLE. It was commissioned into Apple Home and relayed remote lock/unlock and lock state on the bench, and was retired:

| | A: Hub is the Matter node (retired) | **B: Lock is a Matter-over-Thread device (chosen)** |
|---|---|---|
| Boxes to buy and place | Lock, keypad, hub | Lock, keypad |
| Needs a Thread border router | No | Yes. Common already (HomePod mini, Apple TV, Nest Hub, many routers). |
| Lock battery life | Best (BLE, long connection interval) | Good (Thread ICD / sleepy end device) |
| Matter stack on battery MCU | No | Yes |
| Custom protocol to secure | lock↔hub and lock↔keypad | lock↔keypad only |

Fewer boxes and less custom protocol matter more for a product people build or buy as a kit than the battery-life edge.

### Open risks of the Thread design

- **BLE coexistence:** *solved on the C6:* Matter's BLE manager owns the NimBLE host and registers the link's GATT service with its own; commissioning advertises on extended advertising set 0 and the keypad link on set 1 (legacy PDUs, public address). Re-check on the H2.
- **Time:** schedules need wall-clock time. Candidates: Matter Time Synchronization from the controller, or SNTP through the border router. Until then the console `time` command stands in.
- **Setup codes:** every unit needs its own passcode, discriminator and QR label (and production DACs), generated at manufacturing.
- **Certification:** selling kits raises CSA certification and vendor ID costs (see §3).

---

## 2. Hardware

### 2.1 Silicon

| Unit | Recommended | Notes |
|---|---|---|
| Lock unit | **ESP32-C6** to prototype → **ESP32-H2** for production (Thread + BLE, no Wi-Fi) | Best in class for battery life and secure storage: **nRF54L15** / **EFR32MG24**. Re-evaluate before production. |
| Keypad | **ESP32-H2** + low-power capacitive touch controller (e.g. Azoteq IQS7222) that wakes the MCU on touch | The H2 and C6 have no touch peripheral. Mechanical keys with GPIO wake are the cheaper alternative. Target: 2×AA or 1×CR123A lasting 2+ years. |

**Prototype kit:** 2× ESP32-C6-DevKitC-1 (the keypad can start on a C6), a 3×4 keypad, a DC gear motor + DRV8833 H-bridge with a current-sense resistor, an AS5600 magnetic encoder, and a reed switch + magnet for the door.

Toolchain: **ESP-IDF v6.0.2 + esp-matter v1.6.1**.

### 2.2 Retrofit mechanism (lock unit)

- **Mounting:** sits over the existing deadbolt's interior side, using the deadbolt's own through-bolts with a per-brand adapter plate. A swappable tailpiece adapter couples to the cylinder's tailpiece.
- **Manual operation always works.** The drivetrain must be backdrivable or have a clutch, so both the interior knob on the unit and the exterior key can turn the bolt without fighting the motor.
- **Absolute position sensing:** a magnetic encoder on the output shaft knows the bolt position no matter who turned it. Key and knob operations are detected and reported as "manual".
- **Jam detection:** motor current spike + encoder stall + timeout.
- **Calibration (app):** door handing, locked/unlocked end angles and torque limit. An auto-calibration routine sweeps to the end stops.
- **Door position sensor:** magnet on the frame, hall/reed sensor in the unit. Auto-relock never throws the bolt while the door is open.
- **Power:** 4×AA lithium, motor peaks around 1–2 A. Low battery is reported early via the Matter Power Source cluster, and the unit refuses motor moves below a safe voltage threshold rather than stalling halfway.
- **Fallback:** the unchanged exterior cylinder means the physical key is always the last resort: dead batteries, radio jamming, firmware bugs.

---

## 3. Privacy model

### Guarantees
- **No cloud dependency, ever.** No vendor servers, accounts, analytics, crash reporting, or "phone home". The lock only speaks Matter on the local Thread network. It has no route to the internet of its own, and nothing is lost if the border router's WAN access is blocked.
- **The iOS app is local-only.** CoreBluetooth only: no network entitlement use, no third-party SDKs, no account.
- **Minimal data at rest.**
  - PINs are stored as `HMAC(device_key, salt‖pin)`, where `device_key` sits in an eFuse HMAC key block that software can't read. A dumped flash image can't be brute-forced offline, which matters because 4–8 digit PINs are easy to brute-force without that hardware binding.
  - The access log lives on the lock unit only, as a bounded ring buffer with configurable retention, including "off".
  - The keypad stores its pairing key only.
- **BLE hygiene.** Rotating resolvable private addresses and generic advertising names. The lock only accepts connections from bonded peers (filter accept list), except during a physical setup window.

### Honest limits (document for users)
- Events sent to a Matter controller fall under that ecosystem's privacy policy (Apple Home: end-to-end encrypted; Google/Amazon: their cloud; Home Assistant: local). The **private events** setting drops *who* unlocked (`UserIndex`) from everything that leaves the lock unit.
- Production Matter devices need CSA certification and a Device Attestation Certificate. Development uses test DACs.

---

## 4. Security design

### 4.1 Hardware hardening (all MCUs)
- Secure Boot v2, flash encryption (release mode), NVS encryption via HMAC-derived keys.
- JTAG and UART download disabled via eFuse in production.
- Evaluate **ESP-TEE** on the C6 to isolate credential verification.
- Signed OTA images only, with anti-rollback via secure version eFuse.

### 4.2 Wireless keypad
- **What possession of a keypad gets an attacker:** the right to *submit PIN guesses*. Nothing else. Guesses go through the lock's persistent exponential backoff (`WrongCodeEntryLimit` / `UserCodeTemporaryDisableTime`), which survives battery pulls.
- **Pairing:** only during a physical setup window on the lock unit, which is inside the house. An attacker can't pair a rogue keypad from outside.
- **Swapping in a skimmer:** a replacement keypad can't pair, so it can't produce real lock feedback. If the paired keypad stops checking in or reports tamper (backplate switch), the lock raises a tamper event to the app and Matter.
- **Transport:** each PIN attempt goes in an encrypted, replay-protected message on the Noise channel (§4.4). PINs never appear in advertisements, and the keypad wipes its key buffer immediately after sending.
- **Responsiveness:** the keypad wakes on the first touch and connects while the user keeps typing. The lock listens for bonded peers at a ~1 s interval, so connecting should be done before the last digit.
- **Anti-peep (optional):** accept the correct PIN anywhere inside a longer digit string.
- **Jamming:** a 2.4 GHz jammer can block the keypad (a denial of service only). The physical key still works.

### 4.3 Matter (remote) access
- **Lock-side policy caps Matter.** `remote_unlock_allowed` and `require_pin_for_remote` are lock-unit settings that Matter controllers can't change. A compromised controller can't open the door when remote unlock is off.
- **Latency:** a Thread sleepy end device polls its parent; the poll interval trades battery for remote-unlock latency. Target ≤1 s while active.

### 4.4 Secure channel (shared by keypad and app)
- An app-layer **Noise** channel (X25519, ChaCha20-Poly1305, SHA-256). Sessions use **IK**: one round trip, mutual authentication, and the initiator's identity is encrypted. Pairing uses **XX**. BLE-layer security isn't relied on. See [LINK_PROTOCOL.md](LINK_PROTOCOL.md).
- Static keys are exchanged during pairing. Keypads pair inside the physical window ("just works"). Phone pairing will add a confirmation code derived from the handshake hash.
- Every peer has a role (`keypad`, `admin_phone`, `user_phone`) the lock enforces. Revoke a peer from an admin phone or with a factory reset.

---

## 5. What goes through Matter vs. the app

| Feature | Matter (ecosystem app) | iOS app |
|---|---|---|
| Lock / unlock / state | ✅ | ✅ (fallback, Thread offline) |
| Users and PIN codes | ✅ `SetUser` / `SetCredential` | ✅ (fallback) |
| Week-day schedules | ✅ (varies by ecosystem) | ✅ |
| Auto-relock, volume, operating modes | ✅ `AutoRelockTime`, `SoundVolume`, `OperatingMode` | – |
| Battery | ✅ Power Source cluster | ✅ |
| Firmware updates | ✅ Matter OTA (lock) | ✅ keypad OTA over BLE |
| Motor calibration | ❌ | ✅ |
| Keypad pairing, phone enrollment | ❌ | ✅ |
| Local access log, diagnostics, factory reset | ❌ | ✅ |

iOS app stack: SwiftUI + CoreBluetooth, with the protocol in a standalone Swift package so it can be tested against firmware test vectors. The Kotlin app reuses the same protocol spec and vectors later.

Watch CSA's **Aliro** standard (phone-as-key via NFC/BLE/UWB). It could later replace parts of the app with OS-native wallet keys.

---

## 6. Repository layout

```
firmware/
  components/
    lock_core/        # Pure C11, no ESP-IDF deps: users/PINs/schedules, rate limiting,
                      #   access log, lock state machine and policy. Host unit-tested.
    secure_link/      # Noise IK/XX channel (keypad/app)
    link_proto/       # Versioned message definitions + test vectors
  apps/
    components/       # ESP-only: platform_esp (NVS/eFuse-HMAC/time adapters for
                      #   lock_core), ln_store (NVS, secrets), ln_central (BLE central)
    lock_unit/        # ESP-IDF: lock_core + motor/encoder + BLE peripheral
                      #   (+ Matter-over-Thread Door Lock endpoint, next)
    keypad/           # ESP-IDF: touch → PIN attempt → secure_link client
  test/host/          # CMake/CTest suite for the pure-C components
ios/                  # Swift app + LochnessProtocol Swift package
android/              # later
hardware/             # schematics, mechanical CAD, adapter plates
docs/
```

---

## 7. Milestones

1. **M0 – Core logic on host:** `lock_core` (users, PINs, schedules, rate limiter, access log, lock state machine, operating modes, auto-relock) with host tests. ✅
2. **M1 – Standalone lock:** lock unit on a C6 devkit. ← *in progress: app, NVS persistence, PIN digest, door sensor and console done; motor/encoder driver pending hardware.* Motor + encoder + door sensor, NVS persistence, a serial console standing in for the keypad.
3. **M2 – Secure link + wireless keypad:** `secure_link` + `link_proto`, keypad firmware on a second devkit, pairing flow. ← *first pass done: Noise IK/XX verified against test vectors, lock-side server, keypad state machine, end-to-end host tests, BLE glue for both apps (built, not yet run on hardware).*
4. **M3 – Matter over Thread on the lock:** esp-matter Door Lock endpoint in the lock unit itself, commissioned into Apple Home / Home Assistant through a Thread border router. ← *in progress: Door Lock endpoint (lock/unlock, LockState, operation and alarm events) running on the C6 next to the keypad link; Apple Home commissioning next. Then users/PINs, time sync and low-power Thread. A Wi-Fi hub prototype of this milestone (a third devkit bridging Matter to the lock over BLE) was commissioned into Apple Home, then retired for the hubless design.*
5. **M4 – iOS app:** enrollment, calibration, local log, offline lock/unlock.
6. **M5 – Hardening + power:** secure boot, flash/NVS encryption, HMAC-bound PINs, power profiling of the lock and keypad.
7. **M6 – Production path:** mechanical design + adapter plates, custom PCBs, re-evaluate silicon, CSA certification.

---

## 8. Open decisions

- [ ] Production lock/keypad silicon: ESP32-H2 vs. nRF54L15 / EFR32MG24.
