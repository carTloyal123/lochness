/*
 * lochness link protocol v1: framing and messages exchanged between the lock
 * unit and its peers (keypad now; hub and phone later).
 *
 * Frame (one BLE GATT write / notification):
 *   [frame_type u8][body]
 *   IK1/IK2/XX1..3 bodies are Noise handshake messages; DATA bodies are Noise
 *   transport ciphertexts. Only the frame type is visible on the air.
 *
 * Messages (the plaintext inside handshake payloads and DATA frames):
 *   [msg_type u8][fixed-size body], little-endian. Bodies have a fixed size per
 *   type so ciphertext length reveals nothing (e.g. PIN length, lock vs unlock).
 *
 * The wire format is the contract with the iOS/Android apps. Keep encode/decode
 * here and add test vectors when anything changes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LP_PROTO_VERSION 1
#define LP_MAX_FRAME 200
#define LP_PIN_FIELD_LEN 8
/* Bound into every Noise handshake; a version mismatch fails authentication. */
#define LP_PROLOGUE "lochness-link/1"
#define LP_PROLOGUE_LEN (sizeof(LP_PROLOGUE) - 1)

typedef enum {
    LP_FRAME_IK1 = 0x01, /* initiator -> lock: Noise IK message 1 (session) */
    LP_FRAME_IK2 = 0x02,
    LP_FRAME_XX1 = 0x11, /* initiator -> lock: Noise XX message 1 (pairing) */
    LP_FRAME_XX2 = 0x12,
    LP_FRAME_XX3 = 0x13,
    LP_FRAME_DATA = 0x20,
} lp_frame_t;

typedef enum {
    LP_ROLE_KEYPAD = 1,
    LP_ROLE_HUB = 2,
    LP_ROLE_PHONE = 3,
} lp_role_t;

typedef enum {
    LP_MSG_HELLO = 0x01,         /* peer -> lock, in IK1 / XX3 payload */
    LP_MSG_HELLO_ACK = 0x02,     /* lock -> peer, in IK2 payload */
    LP_MSG_ACCESS_REQ = 0x10,    /* peer -> lock */
    LP_MSG_ACCESS_RESULT = 0x11, /* lock -> peer */
    LP_MSG_KEYPAD_STATUS = 0x20, /* keypad -> lock */
    LP_MSG_PAIR_RESULT = 0x30,   /* lock -> peer, first DATA after XX */
} lp_msg_type_t;

typedef enum { LP_OP_LOCK = 0, LP_OP_UNLOCK = 1 } lp_op_t;

/* Stable on-wire result codes (independent of lock_core's enum values). */
typedef enum {
    LP_RESULT_OK = 0,
    LP_RESULT_DENIED = 1,
    LP_RESULT_LOCKED_OUT = 2,
    LP_RESULT_SCHEDULE = 3,
    LP_RESULT_MODE = 4,
    LP_RESULT_BUSY = 5,
    LP_RESULT_DOOR_OPEN = 6,
    LP_RESULT_NOT_PERMITTED = 7, /* this peer's role may not do that */
    LP_RESULT_ERROR = 8,
} lp_result_t;

typedef enum {
    LP_BOLT_UNKNOWN = 0,
    LP_BOLT_LOCKED = 1,
    LP_BOLT_UNLOCKED = 2,
    LP_BOLT_MOVING = 3,
    LP_BOLT_JAMMED = 4,
} lp_bolt_t;

#define LP_STATUS_TAMPER 0x01
#define LP_STATUS_LOW_BATTERY 0x02

typedef struct {
    uint8_t type; /* lp_msg_type_t */
    union {
        struct {
            uint8_t proto;
            uint8_t role;
            uint8_t fw_major, fw_minor;
            uint16_t battery_mv; /* 0 = mains / unknown */
        } hello;
        struct {
            uint8_t proto;
            uint8_t bolt;
        } hello_ack;
        struct {
            uint8_t seq;
            uint8_t op;
            uint8_t pin_len; /* 0 = no PIN (e.g. one-touch lock) */
            uint8_t pin[LP_PIN_FIELD_LEN]; /* ASCII digits, zero padded */
        } access_req;
        struct {
            uint8_t seq;
            uint8_t result;
            uint8_t bolt;
            uint16_t lockout_s; /* remaining keypad lockout, if any */
        } access_result;
        struct {
            uint16_t battery_mv;
            uint8_t flags;
        } keypad_status;
        struct {
            uint8_t accepted;
        } pair_result;
    };
} lp_msg_t;

/* Returns the encoded length, or 0 if msg is invalid or cap is too small. */
size_t lp_encode(const lp_msg_t *msg, uint8_t *buf, size_t cap);
/* Strict: exact length per type, validated fields. Returns false if malformed. */
bool lp_decode(const uint8_t *buf, size_t len, lp_msg_t *msg);

void lp_msg_wipe(lp_msg_t *msg);

#ifdef __cplusplus
}
#endif
