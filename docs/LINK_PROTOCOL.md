# Lochness link protocol v1

This is how keypads (and later the hub and phone apps) talk to the lock unit. The source of truth is the code: `firmware/components/link_proto/include/link_proto/lp.h` (messages) and `lp_ble.h` (BLE mapping). This doc explains it for app developers.

## Layers

```
 message      [type u8][fixed-size body]          link_proto (lp.h)
 secure       Noise IK / XX, ChaCha20-Poly1305    secure_link (sl_noise.h)
 frame        [frame_type u8][noise bytes]        one GATT write / notification
 transport    BLE GATT                            lp_ble.h
```

## Cryptography

| | |
|---|---|
| Sessions | `Noise_IK_25519_ChaChaPoly_SHA256`. The initiator already knows the lock's static key. |
| Pairing | `Noise_XX_25519_ChaChaPoly_SHA256`. Static keys are exchanged. |
| Prologue | ASCII `lochness-link/1`. A version mismatch fails the handshake. |
| Transport nonces | Implicit counters (Noise default). BLE delivery is reliable and ordered, so any replayed, dropped or reordered frame fails to decrypt and ends the session. |

The firmware implementation matches the [cacophony](https://github.com/haskell-cryptography/cacophony) IK and XX test vectors byte for byte (`firmware/test/host/test_secure_link.c`). An app implementation should pass the same vectors. On iOS, CryptoKit provides everything needed: `Curve25519.KeyAgreement`, `ChaChaPoly` and `SHA256`/`HMAC`.

**Never put actions in IK message 1.** Its payload can be replayed by an attacker. It carries only `HELLO`.

## BLE transport

- The lock is the GATT server. Service `68af0681-139d-4a1a-9909-d52a69a3cae6`.
  - RX `68af0682-…` (write / write without response): peer → lock
  - TX `68af0683-…` (notify): lock → peer
- The peer must exchange MTU (≥ 203) and subscribe to TX before sending its first frame.
- Advertising carries 128-bit service data: the service UUID plus one flags byte. Bit 0 set means a pairing window is open. No device name is advertised.

## Frames

| Type | Direction | Body |
|---|---|---|
| `0x01` IK1 | peer → lock | Noise IK msg 1, payload `HELLO` |
| `0x02` IK2 | lock → peer | Noise IK msg 2, payload `HELLO_ACK` |
| `0x11` XX1 | peer → lock | Noise XX msg 1, empty payload |
| `0x12` XX2 | lock → peer | Noise XX msg 2, empty payload |
| `0x13` XX3 | peer → lock | Noise XX msg 3, payload `HELLO` |
| `0x20` DATA | both | Noise transport message |

Maximum frame size is 200 bytes. On any unexpected frame or authentication failure, the lock drops the connection without explanation.

## Messages

All integers are little-endian. Every type has a fixed size, so ciphertext length reveals nothing (for example, PIN length or lock vs. unlock).

| Type | Name | Body |
|---|---|---|
| `0x01` | HELLO | proto u8, role u8 (1 keypad, 2 hub, 3 phone), fw_major u8, fw_minor u8, battery_mv u16 |
| `0x02` | HELLO_ACK | proto u8, bolt u8 |
| `0x10` | ACCESS_REQ | seq u8, op u8 (0 lock, 1 unlock), pin_len u8, pin[8] ASCII digits, zero padded |
| `0x11` | ACCESS_RESULT | seq u8, result u8, bolt u8, lockout_s u16 |
| `0x20` | KEYPAD_STATUS | battery_mv u16, flags u8 (bit 0 tamper, bit 1 low battery) |
| `0x30` | PAIR_RESULT | accepted u8 |
| `0x40` | TIME_SET | unix_s u32, utc_offset_min i16 (local offset now, DST included; −720..840) |
| `0x41` | PING | (empty) |
| `0x50` | LOCK_STATE | bolt u8, door u8, event u8, source u8, user_index u16 |

Result codes: 0 ok, 1 denied, 2 locked out, 3 outside schedule, 4 mode/policy, 5 busy, 6 door open, 7 not permitted for role, 8 error.
Bolt states: 0 unknown, 1 locked, 2 unlocked, 3 moving, 4 jammed.
Door states: 0 unknown, 1 closed, 2 open.
Events: 0 locked, 1 unlocked, 2 wrong code, 3 lockout started, 4 access denied, 5 jammed, 6 door opened, 7 door closed, 255 none (state snapshot).
Sources: 0 keypad, 1 remote (hub), 2 app, 3 manual, 4 auto-relock, 255 none.

`battery_mv` 0 means unknown (no sense circuit). A keypad sends `KEYPAD_STATUS` only when it has a flag to report, at the start of a session.

## Flows

**Pairing (keypad).** Someone presses the pair button on the lock unit, which is inside the house. The lock opens a 60-second window for one role. The keypad, held in pairing mode, scans for the pairing flag, connects, and runs XX. The lock stores the keypad's static key with that role, closes the window, and sends `PAIR_RESULT`. The keypad stores the lock's static key and BLE address. Each window allows one pairing.

**Session.** On the first key press the keypad connects and sends IK1 with `HELLO`. The lock looks up the decrypted static key. If it's an unknown key or the role doesn't match, the lock drops the connection. Otherwise it replies with IK2. The keypad then sends `ACCESS_REQ` as DATA and gets `ACCESS_RESULT` back. The lock drops idle sessions after 30 s and unfinished handshakes after 5 s.

**Hub session.** The hub pairs like a keypad, inside a window opened for the hub role (`pair hub`), and keeps the XX session as its first session. Afterwards it stays connected: it reconnects with backoff (0.5 s doubling to 30 s) and sends `TIME_SET`, or `PING` while it has no clock, every 60 s. The lock drops a hub session after 5 minutes of silence. Right after a hub session is established the lock sends a `LOCK_STATE` snapshot (event 255); after that it pushes `LOCK_STATE` on every event and every bolt or door change. Only hub (and later phone) sessions receive `LOCK_STATE`, and only they may set the clock: a keypad sits outside the door, and moving the clock would move every schedule.

**Keypad resend.** If the link drops after a keypad sent `ACCESS_REQ` but before `ACCESS_RESULT` arrived (for example because the lock rebooted), the keypad resends it once in a new session. Lock and unlock are idempotent. A wrong PIN whose answer was lost may count twice toward the lockout.

**Roles.** The lock maps roles to its access rules: keypad requests follow the PIN rules and rate limit, hub requests follow the remote policy (`remote_unlock_allowed`, `require_pin_for_remote`), and phone requests count as app operations.

## Known gaps (first pass)

- **Privacy:** the lock advertises a fixed service UUID from a stable address. Planned: rotating addresses, plus an advertising tag only paired peers can recognize.
- **Pairing confirmation:** keypad pairing is "just works" inside the physical window. A rogue keypad paired during the window can only submit rate-limited PIN guesses. Hub and phone pairing will add a confirmation code derived from the handshake hash.
- **Events to phones:** `LOCK_STATE` goes to hub and phone sessions; there is no phone app yet.
- **Low battery and tamper** from keypads are logged on the lock but not yet raised as events to the hub.
