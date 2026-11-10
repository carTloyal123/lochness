/*
 * Shared types and compile-time limits for lock_core.
 *
 * Indices for users and credentials are 1-based to match the Matter Door Lock
 * cluster; 0 means "none".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LC_MAX_USERS
#define LC_MAX_USERS 32
#endif
#ifndef LC_MAX_CREDENTIALS
#define LC_MAX_CREDENTIALS 64
#endif
#ifndef LC_MAX_WEEKDAY_SCHEDULES
#define LC_MAX_WEEKDAY_SCHEDULES 4 /* per user */
#endif
#ifndef LC_LOG_CAPACITY
#define LC_LOG_CAPACITY 256
#endif

#define LC_PIN_MIN_LEN 4
#define LC_PIN_MAX_LEN 8
#define LC_SALT_LEN 16
#define LC_DIGEST_LEN 32
#define LC_USER_NAME_MAX 10 /* Matter UserName max length */
#define LC_INDEX_NONE 0

typedef enum {
    LC_OK = 0,
    LC_ERR_INVALID_ARG,
    LC_ERR_NOT_FOUND,
    LC_ERR_FULL,
    LC_ERR_DUPLICATE,  /* PIN already in use by another credential */
    LC_ERR_DENIED,     /* wrong, unknown, disabled or non-access credential */
    LC_ERR_LOCKED_OUT, /* too many wrong codes; PIN was not evaluated */
    LC_ERR_SCHEDULE,   /* valid credential outside its schedule (or time unknown) */
    LC_ERR_MODE,       /* operating mode or lock-side policy forbids this */
    LC_ERR_BUSY,       /* bolt already moving */
    LC_ERR_DOOR_OPEN,  /* refusing to throw the bolt into an open door */
    LC_ERR_PLATFORM,   /* platform hook failed */
} lc_err_t;

typedef enum {
    LC_SOURCE_KEYPAD,
    LC_SOURCE_REMOTE, /* via Matter */
    LC_SOURCE_APP,    /* phone over the secure channel; authorized at channel layer */
    LC_SOURCE_MANUAL, /* thumb-turn knob or exterior key, detected by encoder */
    LC_SOURCE_AUTO,   /* auto-relock */
} lc_source_t;

typedef enum {
    LC_OP_LOCK,
    LC_OP_UNLOCK,
} lc_op_t;

/* Wall-clock time, as Matter Time Sync (or the app) supplies it. */
typedef struct {
    uint32_t unix_s;
    uint8_t weekday;        /* 0 = Sunday ... 6 = Saturday, local time */
    uint16_t minute_of_day; /* 0..1439, local time */
} lc_wall_time_t;

/*
 * Services lock_core needs from the platform. All function pointers except
 * wall_time are required.
 */
typedef struct {
    void *ctx;
    /* Monotonic milliseconds since boot. */
    uint64_t (*now_ms)(void *ctx);
    /* Current local wall time; return false if unknown. May be NULL. */
    bool (*wall_time)(void *ctx, lc_wall_time_t *out);
    /*
     * Keyed digest of salt || pin. In production this MUST be HMAC-SHA256
     * keyed by a hardware key the CPU cannot read (ESP32 eFuse HMAC block),
     * so stored digests cannot be brute-forced off-device.
     */
    lc_err_t (*pin_digest)(void *ctx, const uint8_t salt[LC_SALT_LEN], const uint8_t *pin,
                           size_t pin_len, uint8_t out[LC_DIGEST_LEN]);
    /* Cryptographically secure random bytes. */
    lc_err_t (*random)(void *ctx, uint8_t *buf, size_t len);
} lc_platform_t;

#ifdef __cplusplus
}
#endif
