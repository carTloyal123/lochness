/*
 * Local access log: a fixed-size ring buffer that lives only on the lock unit.
 * Retention is bounded by capacity and an optional maximum age; logging can be
 * disabled entirely.
 */
#pragma once

#include "lock_core/lc_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LC_EVENT_LOCKED,
    LC_EVENT_UNLOCKED,
    LC_EVENT_WRONG_CODE,
    LC_EVENT_LOCKOUT_STARTED,
    LC_EVENT_ACCESS_DENIED, /* valid credential refused: schedule, disabled, non-access, mode */
    LC_EVENT_JAMMED,
    LC_EVENT_DOOR_OPENED,
    LC_EVENT_DOOR_CLOSED,
} lc_event_type_t;

typedef struct {
    uint32_t unix_s;   /* 0 if wall time was unknown */
    uint32_t uptime_s; /* seconds since boot, always set */
    uint8_t type;      /* lc_event_type_t */
    uint8_t source;    /* lc_source_t */
    uint16_t user_index;
    /* Boot counter: with uptime_s, orders entries that have no wall time. */
    uint16_t boot;
    uint16_t reserved;
} lc_log_entry_t;

typedef struct {
    bool enabled;
    uint16_t max_age_days; /* 0 = keep until overwritten */
} lc_log_cfg_t;

typedef struct {
    lc_log_entry_t entries[LC_LOG_CAPACITY];
    uint16_t head;  /* next write position */
    uint16_t count;
} lc_log_t;

void lc_log_init(lc_log_t *log);
void lc_log_append(lc_log_t *log, const lc_log_cfg_t *cfg, const lc_log_entry_t *entry);
/* i = 0 is the oldest entry. Returns NULL if i >= count. */
const lc_log_entry_t *lc_log_get(const lc_log_t *log, uint16_t i);
/* Drop entries older than max_age_days relative to now_unix_s. Entries without
 * wall time are kept (their age is unknown). Returns number removed. */
uint16_t lc_log_prune(lc_log_t *log, const lc_log_cfg_t *cfg, uint32_t now_unix_s);

#ifdef __cplusplus
}
#endif
