# Lochness: Architecture (Draft v1)

A local-first smart lock. Two principles drive the design, in this order:

1. **Maximum user privacy.** No cloud, no accounts, no telemetry. The lock is fully functional with zero internet access.
2. **Ease of use.** Normal use happens through the user's existing Matter ecosystem (Apple Home, Google Home, Home Assistant, SmartThings, …). A companion app exists only for things Matter cannot do.

### Decisions so far

| Decision | Choice |
|---|---|
| Topology | **Hub is the Matter device** (Matter over Wi-Fi). Lock and keypad talk to it over BLE. |
| Keypad | **Always wireless**, battery powered, mounted outside |
| Lock form factor | **Retrofit**: replaces the interior thumb-turn. The exterior cylinder and key stay unchanged. **US single-cylinder deadbolts first** (Kwikset, Schlage, Yale and similar tailpieces). |
| Keypad input | **Mechanical buttons and capacitive touch**, behind one input-driver interface in the keypad firmware |
| Toolchain | **ESP-IDF v6.0.2** (pinned to esp-matter's recommended version). PlatformIO was rejected because esp-matter doesn't support it. |
| Companion app | **Native**: Swift (iOS) first, Kotlin (Android) later |

---

## 1. System topology

```
   EXTERIOR (lock face + key unchanged) │  INTERIOR                                       (anywhere in Wi-Fi range)
                                        │
  ┌───────────────────────┐   BLE       │  ┌──────────────────────────────┐   BLE link    ┌────────────────────────┐
  │ Wireless keypad       │  secure     │  │ Lock unit (battery)          │◄─────────────►│ Hub (mains powered)    │
  │ - battery, deep sleep │  channel    │  │ replaces interior thumb-turn │  secure       │ - Matter over Wi-Fi    │
  │ - sends PIN attempts  ├─────────────┼─►│ - credential DB (truth)      │  channel      │ - Door Lock endpoint   │
  │ - holds NO PINs       │             │  │ - every access decision      │               │ - time source          │
  │ - tamper switch       │             │  │ - motor, encoder, door sensor│               │ - NO credential store  │
  └───────────────────────┘             │  └──────────────▲───────────────┘               └───────────▲────────────┘
                                        │                 │ BLE (setup / advanced only)               │ Matter (local IP)
                                        │          ┌──────┴──────┐                          ┌─────────┴──────────┐
                                        │          │  iOS app    │                          │ Apple/Google/HA/…  │
                                        │          └─────────────┘                          └────────────────────┘
```

The keypad talks **directly to the lock unit**, not through the hub. Keypad entry keeps working when the hub or Wi-Fi is down.

### Responsibilities

| Unit | Role | Trust level |
|---|---|---|
| **Keypad** | Captures key presses and sends PIN attempts to the lock over an encrypted channel. Drives LEDs and the buzzer from the lock's reply. Holds no PINs and cannot unlock without a correct PIN. Outside the door, so assume attackers can steal or replace it. | Untrusted input device |
| **Lock unit** | Security root. Stores users, credentials and schedules. Makes every lock/unlock decision, drives the motor, enforces rate limits and keeps the access log. Works fully standalone. | Root of trust |
| **Hub** | Matter endpoint. Translates Door Lock cluster commands into link messages to the lock unit. Supplies wall-clock time. Stores nothing about users at rest. | Trusted, but capped by lock-side policy |
| **App** | Setup, calibration, diagnostics and recovery, over BLE straight to the lock unit. | Per-phone keys. Enrollment requires physical access. |

### Why the hub is the Matter node

| | **A: Hub is the Matter node (chosen)** | B: Lock is a Matter-over-Thread device |
|---|---|---|
| Needs a Thread border router | No. Plain Wi-Fi is enough. | Yes |
| Lock battery life | Best (BLE, long connection interval) | Good (Thread ICD) |
| Matter stack on battery MCU | No | Yes |
| Custom protocol to secure | Yes (lock↔hub, lock↔keypad) | lock↔keypad only |

`lock_core` doesn't depend on the topology, so B stays available as a future SKU.

---

## 2. Hardware

### 2.1 Silicon

| Unit | Recommended | Notes |
|---|---|---|
| Hub | **ESP32-C6** | Use the **ESP32-C5** if 5 GHz Wi-Fi matters. The C6's 802.15.4 radio leaves room to act as a Thread border router later. |
| Lock unit | **ESP32-C6** to prototype → evaluate **ESP32-H2** for production | Best in class for battery life and secure storage: **nRF54L15** / **EFR32MG24**. Re-evaluate before production. |
| Keypad | **ESP32-H2** + low-power capacitive touch controller (e.g. Azoteq IQS7222) that wakes the MCU on touch | The H2 and C6 have no touch peripheral. Mechanical keys with GPIO wake are the cheaper alternative. Target: 2×AA or 1×CR123A lasting 2+ years. |

**Prototype kit:** 3× ESP32-C6-DevKitC-1 (the keypad can start on a C6), a 3×4 keypad, a DC gear motor + DRV8833 H-bridge with a current-sense resistor, an AS5600 magnetic encoder, and a reed switch + magnet for the door.

Toolchain: **ESP-IDF v5.x + esp-matter**.

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
- **No cloud dependency, ever.** No vendor servers, accounts, analytics, crash reporting, or "phone home". The hub only uses local IP (mDNS + Matter). Users can block the hub's WAN access at the router and lose nothing.
- **The iOS app is local-only.** CoreBluetooth only: no network entitlement use, no third-party SDKs, no account.
- **Minimal data at rest.**
  - PINs are stored as `HMAC(device_key, salt‖pin)`, where `device_key` sits in an eFuse HMAC key block that software can't read. A dumped flash image can't be brute-forced offline, which matters because 4–8 digit PINs are easy to brute-force without that hardware binding.
  - The access log lives on the lock unit only, as a bounded ring buffer with configurable retention, including "off".
  - The hub stores Matter fabric credentials only. The keypad stores its pairing key only.
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
- **Swapping in a skimmer:** a replacement keypad can't pair, so it can't produce real lock feedback. If the paired keypad stops checking in or reports tamper (backplate switch), the lock raises a tamper event to the app and hub.
- **Transport:** each PIN attempt goes in an encrypted, replay-protected message on the Noise channel (§4.4). PINs never appear in advertisements, and the keypad wipes its key buffer immediately after sending.
- **Responsiveness:** the keypad wakes on the first touch and connects while the user keeps typing. The lock listens for bonded peers at a ~1 s interval, so connecting should be done before the last digit.
- **Anti-peep (optional):** accept the correct PIN anywhere inside a longer digit string.
- **Jamming:** a 2.4 GHz jammer can block the keypad (a denial of service only). The physical key still works.

### 4.3 Lock ↔ hub link
- **Pairing:** physical setup window on both units, or factory-bonded kits.
- **Lock-side policy caps the hub.** `remote_unlock_allowed` is a lock-unit setting the hub can't change. A fully compromised hub can't open the door when it's off.
- **Latency:** persistent connection at a ~500 ms–1 s interval with peripheral latency, giving ≤1 s remote unlock at tens of µA average.

### 4.4 Secure channel (shared by hub, keypad and app)
- BLE LE Secure Connections, plus an app-layer **Noise `KK`** channel (X25519, ChaCha20-Poly1305, SHA-256) with monotonic counters. BLE-layer security alone isn't trusted.
- Static keys are exchanged during pairing, confirmed by a short authentication code (shown on the keypad LEDs, the app screen or the hub LED).
- Every peer has a role (`hub`, `keypad`, `admin_phone`, `user_phone`) the lock enforces. Revoke a peer from an admin phone or with a factory reset.

---

## 5. What goes through Matter vs. the app

| Feature | Matter (ecosystem app) | iOS app |
|---|---|---|
| Lock / unlock / state | ✅ | ✅ (fallback, hub offline) |
| Users and PIN codes | ✅ `SetUser` / `SetCredential` | ✅ (fallback) |
| Week-day schedules | ✅ (varies by ecosystem) | ✅ |
| Auto-relock, volume, operating modes | ✅ `AutoRelockTime`, `SoundVolume`, `OperatingMode` | – |
| Battery | ✅ Power Source cluster | ✅ |
| Firmware updates | ✅ Matter OTA (hub) | ✅ lock and keypad OTA over BLE |
| Motor calibration | ❌ | ✅ |
| Keypad pairing, phone enrollment, hub pairing | ❌ | ✅ |
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
    secure_link/      # Noise KK channel (hub/keypad/app)
    link_proto/       # Versioned message definitions + test vectors
    platform_esp/     # NVS/eFuse-HMAC/BLE/GPIO/motor adapters for lock_core
  apps/
    lock_unit/        # ESP-IDF: lock_core + motor/encoder + BLE peripheral
    keypad/           # ESP-IDF: touch → PIN attempt → secure_link client
    hub/              # esp-matter: Door Lock endpoint → link_proto client
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
3. **M2 – Secure link + wireless keypad:** `secure_link` + `link_proto`, keypad firmware on a second devkit, pairing flow.
4. **M3 – Matter hub:** esp-matter Door Lock on a third devkit, commissioned into Apple Home / Home Assistant, driving the real lock over the link.
5. **M4 – iOS app:** enrollment, calibration, local log, offline lock/unlock.
6. **M5 – Hardening + power:** secure boot, flash/NVS encryption, HMAC-bound PINs, power profiling of the lock and keypad.
7. **M6 – Production path:** mechanical design + adapter plates, custom PCBs, re-evaluate silicon, CSA certification.

---

## 8. Open decisions

- [ ] Production lock/keypad silicon: ESP32-H2 vs. nRF54L15 / EFR32MG24.
