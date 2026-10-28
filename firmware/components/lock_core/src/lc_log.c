#include "lock_core/lc_log.h"
#include "lock_core/lc_types.h"

#include <stdint.h>
#include <string.h>

static const uint32_t SECONDS_PER_DAY = 86400U;

static uint16_t oldest_pos(const lc_log_t *log)
{
    return (uint16_t)((log->head + LC_LOG_CAPACITY - log->count) % LC_LOG_CAPACITY);
}

void lc_log_init(lc_log_t *log) { memset(log, 0, sizeof(*log)); }

void lc_log_append(lc_log_t *log, const lc_log_cfg_t *cfg, const lc_log_entry_t *entry)
{
    if (!cfg->enabled) {
        return;
    }
    log->entries[log->head] = *entry;
    log->head = (uint16_t)((log->head + 1) % LC_LOG_CAPACITY);
    if (log->count < LC_LOG_CAPACITY) {
        log->count++;
    }
}

const lc_log_entry_t *lc_log_get(const lc_log_t *log, uint16_t i)
{
    if (i >= log->count) {
        return NULL;
    }
    return &log->entries[(oldest_pos(log) + i) % LC_LOG_CAPACITY];
}

uint16_t lc_log_prune(lc_log_t *log, const lc_log_cfg_t *cfg, uint32_t now_unix_s)
{
    if (!cfg->enabled) {
        uint16_t n = log->count;
        lc_log_init(log);
        return n;
    }
    if (cfg->max_age_days == 0 || now_unix_s == 0) {
        return 0;
    }
    uint32_t max_age_s = (uint32_t)cfg->max_age_days * SECONDS_PER_DAY;
    uint16_t start = oldest_pos(log);
    uint16_t kept = 0;

    /* Compact in place in logical order; the write cursor never passes the
     * read cursor. */
    for (uint16_t read_pos = 0; read_pos < log->count; read_pos++) {
        const lc_log_entry_t *e = &log->entries[(start + read_pos) % LC_LOG_CAPACITY];
        bool expired = e->unix_s != 0 && e->unix_s <= now_unix_s && now_unix_s - e->unix_s > max_age_s;
        if (!expired) {
            if (kept != read_pos) {
                log->entries[(start + kept) % LC_LOG_CAPACITY] = *e;
            }
            kept++;
        }
    }
    uint16_t removed = (uint16_t)(log->count - kept);
    for (uint16_t i = kept; i < log->count; i++) {
        memset(&log->entries[(start + i) % LC_LOG_CAPACITY], 0, sizeof(lc_log_entry_t));
    }
    log->count = kept;
    log->head = (uint16_t)((start + kept) % LC_LOG_CAPACITY);
    return removed;
}
