#include "console.h"

#include <errno.h> // NOLINT(misc-include-cleaner): portable home of errno (picolibc defines it in sys/errno.h)
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_console.h"
#include "esp_system.h"
#include "link_proto/lp.h"
#include "lock_core/lc_creds.h"
#include "lock_core/lc_lock.h"
#include "lock_core/lc_log.h"
#include "lock_core/lc_types.h"
#include "lock_link/ll.h"
#include "lock_task.h"
#include "motor_sim.h"
#include "platform_esp/pe_platform.h"

#define PIN_ARG(s) (const uint8_t *)(s), strlen(s)
#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

enum {
    MINUTES_PER_HOUR = 60,
    SECONDS_PER_MINUTE = 60,
    HOURS_PER_DAY = 24,
    MINUTES_PER_DAY = MINUTES_PER_HOUR * HOURS_PER_DAY,
    SECONDS_PER_DAY = 86400,
    MAX_SETTING_S = SECONDS_PER_DAY, /* cap for relock and lockout settings */
    MIN_UTC_OFFSET_MIN = -12 * MINUTES_PER_HOUR,
    MAX_UTC_OFFSET_MIN = 14 * MINUTES_PER_HOUR,
    DEFAULT_LOG_LINES = 20,
    PAIR_DEFAULT_S = 60,
    PAIR_MAX_S = 600,
    CONSOLE_STACK = 6144,
    DECIMAL = 10,
    SCHED_ADD_ARGC = 5, /* sched <user> <days> <from> <to> */
};

static const char *err_name(lc_err_t e)
{
    static const char *names[] = {
        [LC_OK] = "ok",
        [LC_ERR_INVALID_ARG] = "invalid argument",
        [LC_ERR_NOT_FOUND] = "not found",
        [LC_ERR_FULL] = "full",
        [LC_ERR_DUPLICATE] = "PIN already in use",
        [LC_ERR_DENIED] = "denied",
        [LC_ERR_LOCKED_OUT] = "locked out (too many wrong codes)",
        [LC_ERR_SCHEDULE] = "outside schedule",
        [LC_ERR_MODE] = "not allowed in current mode/policy",
        [LC_ERR_BUSY] = "busy",
        [LC_ERR_DOOR_OPEN] = "door is open",
        [LC_ERR_PLATFORM] = "platform error",
    };
    return (unsigned)e < COUNT_OF(names) ? names[e] : "?";
}

static const char *bolt_name(lc_bolt_t bolt)
{
    static const char *names[] = {"unknown", "locked", "unlocked", "locking", "unlocking", "JAMMED"};
    return (unsigned)bolt < COUNT_OF(names) ? names[bolt] : "?";
}

static const char *door_name(lc_door_t door)
{
    static const char *names[] = {"unknown", "closed", "open"};
    return (unsigned)door < COUNT_OF(names) ? names[door] : "?";
}

static const char *const MODE_NAMES[] = {"normal", "vacation", "privacy", "noremote", "passage"};
static const char *const USER_TYPE_NAMES[] = {"unrestricted", "schedule", "disposable", "nonaccess"};
static const char *const SOURCE_NAMES[] = {"keypad", "remote", "app", "manual", "auto"};
static const char *const EVENT_NAMES[] = {"locked",        "unlocked", "wrong-code",  "lockout",
                                          "access-denied", "jammed",   "door-opened", "door-closed"};
/* Indexed by lp_role_t; 2 was the retired hub (an old pairing shows "?"). */
static const char *const ROLE_NAMES[] = {"?", "keypad", "?", "phone"};

static bool parse_role(const char *name, lp_role_t *role)
{
    if (strcmp(name, "keypad") == 0) {
        *role = LP_ROLE_KEYPAD;
    } else if (strcmp(name, "phone") == 0) {
        *role = LP_ROLE_PHONE;
    } else {
        return false;
    }
    return true;
}

static int lookup(const char *s, const char *const *names, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(s, names[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Parse all of `s` as a decimal integer in [min, max]. */
static bool parse_ll(const char *s, long long min, long long max, long long *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    long long value = strtoll(s, &end, DECIMAL);
    if (errno != 0 || *end != '\0' || value < min || value > max) {
        return false;
    }
    *out = value;
    return true;
}

/* parse_ll, printing what was expected on failure. */
static bool parse_arg(const char *what, const char *s, long long min, long long max, long long *out)
{
    if (parse_ll(s, min, max, out)) {
        return true;
    }
    printf("%s must be a number from %lld to %lld (got '%s')\n", what, min, max, s);
    return false;
}

static bool parse_index(const char *what, const char *s, uint16_t max, uint16_t *out)
{
    long long value = 0;
    if (!parse_arg(what, s, 1, max, &value)) {
        return false;
    }
    *out = (uint16_t)value;
    return true;
}

static int report(lc_err_t e)
{
    printf("%s\n", err_name(e));
    return e == LC_OK ? 0 : 1;
}

/* ---- lock / unlock -------------------------------------------------------- */

typedef struct {
    lc_op_t op;
    lc_source_t source;
    const char *pin;
} request_args_t;

static int do_request(lock_ctx_t *ctx, void *arg)
{
    lc_t *lc = &ctx->lc;
    request_args_t *args = arg;
    uint16_t user = 0;
    lc_err_t e = lc_request(lc, args->op, args->source, args->pin ? (const uint8_t *)args->pin : NULL,
                            args->pin ? strlen(args->pin) : 0, &user);
    if (e == LC_OK && user) {
        printf("user %u: ", user);
    }
    return e;
}

static int request(lc_op_t op, lc_source_t source, const char *pin)
{
    request_args_t args = {.op = op, .source = source, .pin = pin};
    return report((lc_err_t)lock_task_call(do_request, &args));
}

static int cmd_pin(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: pin <digits>\n");
        return 1;
    }
    return request(LC_OP_UNLOCK, LC_SOURCE_KEYPAD, argv[1]);
}

static int cmd_lock(int argc, char **argv)
{
    return request(LC_OP_LOCK, LC_SOURCE_KEYPAD, argc > 1 ? argv[1] : NULL);
}

static bool is_lock_or_unlock(const char *s) { return strcmp(s, "lock") == 0 || strcmp(s, "unlock") == 0; }

static int cmd_remote(int argc, char **argv)
{
    if (argc < 2 || !is_lock_or_unlock(argv[1])) {
        printf("usage: remote lock|unlock [pin]\n");
        return 1;
    }
    return request(strcmp(argv[1], "lock") == 0 ? LC_OP_LOCK : LC_OP_UNLOCK, LC_SOURCE_REMOTE,
                   argc > 2 ? argv[2] : NULL);
}

static int cmd_app(int argc, char **argv)
{
    if (argc != 2 || !is_lock_or_unlock(argv[1])) {
        printf("usage: app lock|unlock\n");
        return 1;
    }
    return request(strcmp(argv[1], "lock") == 0 ? LC_OP_LOCK : LC_OP_UNLOCK, LC_SOURCE_APP, NULL);
}

/* ---- users and codes ------------------------------------------------------ */

typedef struct {
    int argc;
    char **argv;
} argv_t;

static int user_list(const lc_t *lc)
{
    for (uint16_t i = 1; i <= LC_MAX_USERS; i++) {
        const lc_user_t *user = lc_user_get(&lc->creds, i);
        if (!user) {
            continue;
        }
        int codes = 0;
        for (int slot = 0; slot < LC_MAX_CREDENTIALS; slot++) {
            codes += lc->creds.creds[slot].in_use && lc->creds.creds[slot].user_index == i;
        }
        printf("%2u  %-10s  %-12s  %s  codes=%d  schedules=%u\n", i, user->name,
               USER_TYPE_NAMES[user->type], user->enabled ? "enabled " : "disabled", codes,
               user->schedule_count);
    }
    return LC_OK;
}

static int user_add(lc_t *lc, const argv_t *args)
{
    int type = args->argc >= 4 ? lookup(args->argv[3], USER_TYPE_NAMES, COUNT_OF(USER_TYPE_NAMES))
                               : LC_USER_UNRESTRICTED;
    if (type < 0) {
        return LC_ERR_INVALID_ARG;
    }
    uint16_t idx = lc_user_next_free(&lc->creds);
    if (idx == LC_INDEX_NONE) {
        return LC_ERR_FULL;
    }
    lc_err_t e = lc_user_set(&lc->creds, idx, (lc_user_type_t)type, args->argv[2]);
    if (e == LC_OK) {
        lc->hooks.persist(lc->hooks.ctx, LC_PERSIST_CREDS);
        printf("user %u: ", idx);
    }
    return e;
}

/* user del|enable|disable <index> */
static int user_update(lc_t *lc, const char *sub, const char *index_arg)
{
    uint16_t idx = 0;
    if (!parse_index("user index", index_arg, LC_MAX_USERS, &idx)) {
        return LC_ERR_INVALID_ARG;
    }
    lc_err_t e = LC_ERR_INVALID_ARG;
    if (strcmp(sub, "del") == 0) {
        e = lc_user_get(&lc->creds, idx) ? lc_user_clear(&lc->creds, idx) : LC_ERR_NOT_FOUND;
    } else if (strcmp(sub, "enable") == 0 || strcmp(sub, "disable") == 0) {
        e = lc_user_set_enabled(&lc->creds, idx, strcmp(sub, "enable") == 0);
    } else {
        return LC_ERR_INVALID_ARG;
    }
    if (e == LC_OK) {
        lc->hooks.persist(lc->hooks.ctx, LC_PERSIST_CREDS);
    }
    return e;
}

static int do_user(lock_ctx_t *ctx, void *arg)
{
    const argv_t *args = arg;
    const char *sub = args->argv[1];
    if (strcmp(sub, "list") == 0) {
        return user_list(&ctx->lc);
    }
    if (strcmp(sub, "add") == 0 && args->argc >= 3) {
        return user_add(&ctx->lc, args);
    }
    if (args->argc != 3) {
        return LC_ERR_INVALID_ARG;
    }
    return user_update(&ctx->lc, sub, args->argv[2]);
}

static int cmd_user(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: user list | add <name> [unrestricted|schedule|disposable|nonaccess]\n"
               "       user del|enable|disable <index>\n");
        return 1;
    }
    argv_t args = {argc, argv};
    lc_err_t e = (lc_err_t)lock_task_call(do_user, &args);
    return strcmp(argv[1], "list") == 0 && e == LC_OK ? 0 : report(e);
}

static int do_code(lock_ctx_t *ctx, void *arg)
{
    lc_t *lc = &ctx->lc;
    const argv_t *args = arg;
    lc_err_t e = LC_ERR_INVALID_ARG;
    uint16_t idx = 0;
    if (strcmp(args->argv[1], "add") == 0 && args->argc == 4) {
        if (!parse_index("user index", args->argv[2], LC_MAX_USERS, &idx)) {
            return LC_ERR_INVALID_ARG;
        }
        uint16_t cred = lc_cred_next_free(&lc->creds);
        if (cred == LC_INDEX_NONE) {
            return LC_ERR_FULL;
        }
        e = lc_pin_set(&lc->creds, &lc->plat, cred, idx, PIN_ARG(args->argv[3]));
        if (e == LC_OK) {
            printf("code %u: ", cred);
        }
    } else if (strcmp(args->argv[1], "del") == 0 && args->argc == 3) {
        if (!parse_index("code index", args->argv[2], LC_MAX_CREDENTIALS, &idx)) {
            return LC_ERR_INVALID_ARG;
        }
        e = lc_pin_clear(&lc->creds, idx);
    } else {
        return LC_ERR_INVALID_ARG;
    }
    if (e == LC_OK) {
        lc->hooks.persist(lc->hooks.ctx, LC_PERSIST_CREDS);
    }
    return e;
}

static int cmd_code(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: code add <user> <pin> | code del <code-index>\n");
        return 1;
    }
    argv_t args = {argc, argv};
    return report((lc_err_t)lock_task_call(do_code, &args));
}

/* "H:MM" or "HH:MM", up to 24:00. */
static bool parse_hhmm(const char *s, uint16_t *out)
{
    const char *colon = strchr(s, ':');
    if (colon == NULL || colon == s || colon - s > 2) {
        return false;
    }
    char hours_str[3] = {0};
    memcpy(hours_str, s, (size_t)(colon - s));
    long long hours = 0;
    long long minutes = 0;
    if (!parse_ll(hours_str, 0, HOURS_PER_DAY, &hours) ||
        !parse_ll(colon + 1, 0, MINUTES_PER_HOUR - 1, &minutes)) {
        return false;
    }
    long long total = (hours * MINUTES_PER_HOUR) + minutes;
    if (total > MINUTES_PER_DAY) {
        return false;
    }
    *out = (uint16_t)total;
    return true;
}

static int do_sched(lock_ctx_t *ctx, void *arg)
{
    lc_t *lc = &ctx->lc;
    const argv_t *args = arg;
    uint16_t user_idx = 0;
    if (!parse_index("user index", args->argv[1], LC_MAX_USERS, &user_idx)) {
        return LC_ERR_INVALID_ARG;
    }
    if (args->argc == 3 && strcmp(args->argv[2], "clear") == 0) {
        lc_err_t e = lc_user_set_schedules(&lc->creds, user_idx, NULL, 0);
        if (e == LC_OK) {
            lc->hooks.persist(lc->hooks.ctx, LC_PERSIST_CREDS);
        }
        return e;
    }
    lc_weekday_schedule_t sched = {0};
    for (const char *day = args->argv[2]; *day; day++) {
        if (*day < '0' || *day > '6') {
            return LC_ERR_INVALID_ARG;
        }
        sched.days_mask |= (uint8_t)(1U << (unsigned)(*day - '0'));
    }
    if (!parse_hhmm(args->argv[3], &sched.start_min) || !parse_hhmm(args->argv[4], &sched.end_min)) {
        printf("times must be HH:MM\n");
        return LC_ERR_INVALID_ARG;
    }
    const lc_user_t *user = lc_user_get(&lc->creds, user_idx);
    if (!user) {
        return LC_ERR_NOT_FOUND;
    }
    if (user->schedule_count >= LC_MAX_WEEKDAY_SCHEDULES) {
        return LC_ERR_FULL;
    }
    lc_weekday_schedule_t all[LC_MAX_WEEKDAY_SCHEDULES];
    memcpy(all, user->schedules, sizeof(all));
    uint8_t n = user->schedule_count;
    all[n++] = sched;
    lc_err_t e = lc_user_set_schedules(&lc->creds, user_idx, all, n);
    if (e == LC_OK) {
        lc->hooks.persist(lc->hooks.ctx, LC_PERSIST_CREDS);
    }
    return e;
}

static int cmd_sched(int argc, char **argv)
{
    if (!(argc == SCHED_ADD_ARGC || (argc == 3 && strcmp(argv[2], "clear") == 0))) {
        printf("usage: sched <user> <days> <HH:MM> <HH:MM>   days: 0=Sun..6=Sat, e.g. 12345\n"
               "       sched <user> clear\n");
        return 1;
    }
    argv_t args = {argc, argv};
    return report((lc_err_t)lock_task_call(do_sched, &args));
}

/* ---- config ---------------------------------------------------------------- */

static bool parse_bool(const char *s, bool *out)
{
    if (strcmp(s, "on") == 0 || strcmp(s, "1") == 0 || strcmp(s, "true") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(s, "off") == 0 || strcmp(s, "0") == 0 || strcmp(s, "false") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static void config_show(const lc_t *lc)
{
    const lc_config_t *cfg = &lc->cfg;
    printf("mode            %s\n", MODE_NAMES[lc->mode]);
    printf("relock          %lu s (0 = off)\n", (unsigned long)cfg->auto_relock_s);
    printf("onetouch        %s\n", cfg->one_touch_lock ? "on" : "off");
    printf("remote-unlock   %s\n", cfg->remote_unlock_allowed ? "on" : "off");
    printf("remote-pin      %s\n", cfg->require_pin_for_remote ? "on" : "off");
    printf("private         %s\n", cfg->private_events ? "on" : "off");
    printf("wrong-limit     %u\n", cfg->ratelimit.limit);
    printf("lockout         %lu s (max %lu s)\n", (unsigned long)cfg->ratelimit.base_lockout_s,
           (unsigned long)cfg->ratelimit.max_lockout_s);
    printf("log             %s, %u days\n", cfg->log.enabled ? "on" : "off", cfg->log.max_age_days);
}

/* On/off settings. Returns false if `key` is not one of them. */
static bool config_bool_field(lc_config_t *cfg, const char *key, bool **field)
{
    if (strcmp(key, "onetouch") == 0) {
        *field = &cfg->one_touch_lock;
    } else if (strcmp(key, "remote-unlock") == 0) {
        *field = &cfg->remote_unlock_allowed;
    } else if (strcmp(key, "remote-pin") == 0) {
        *field = &cfg->require_pin_for_remote;
    } else if (strcmp(key, "private") == 0) {
        *field = &cfg->private_events;
    } else if (strcmp(key, "log") == 0) {
        *field = &cfg->log.enabled;
    } else {
        return false;
    }
    return true;
}

/* Numeric settings. */
static lc_err_t config_set_number(lc_config_t *cfg, const char *key, const char *value)
{
    long long num = 0;
    if (strcmp(key, "relock") == 0) {
        if (!parse_arg("relock", value, 0, MAX_SETTING_S, &num)) {
            return LC_ERR_INVALID_ARG;
        }
        cfg->auto_relock_s = (uint32_t)num;
    } else if (strcmp(key, "wrong-limit") == 0) {
        if (!parse_arg("wrong-limit", value, 1, UINT8_MAX, &num)) {
            return LC_ERR_INVALID_ARG;
        }
        cfg->ratelimit.limit = (uint8_t)num;
    } else if (strcmp(key, "lockout") == 0) {
        if (!parse_arg("lockout", value, 1, MAX_SETTING_S, &num)) {
            return LC_ERR_INVALID_ARG;
        }
        cfg->ratelimit.base_lockout_s = (uint32_t)num;
    } else if (strcmp(key, "log-days") == 0) {
        if (!parse_arg("log-days", value, 0, UINT16_MAX, &num)) {
            return LC_ERR_INVALID_ARG;
        }
        cfg->log.max_age_days = (uint16_t)num;
    } else {
        return LC_ERR_INVALID_ARG;
    }
    return LC_OK;
}

static lc_err_t config_set(lc_t *lc, const char *key, const char *value)
{
    if (strcmp(key, "mode") == 0) {
        int mode = lookup(value, MODE_NAMES, COUNT_OF(MODE_NAMES));
        if (mode < 0) {
            return LC_ERR_INVALID_ARG;
        }
        lc_set_mode(lc, (lc_mode_t)mode);
        return LC_OK; /* TODO: persist mode with the rest of the runtime config */
    }
    bool *field = NULL;
    if (config_bool_field(&lc->cfg, key, &field)) {
        if (!parse_bool(value, field)) {
            return LC_ERR_INVALID_ARG;
        }
        if (field == &lc->cfg.log.enabled && !*field) {
            lc_log_prune(&lc->log, &lc->cfg.log, 0); /* disabling erases history */
            lc->hooks.persist(lc->hooks.ctx, LC_PERSIST_LOG);
        }
    } else {
        lc_err_t e = config_set_number(&lc->cfg, key, value);
        if (e != LC_OK) {
            return e;
        }
    }
    lock_task_save_config(lc);
    return LC_OK;
}

static int do_config(lock_ctx_t *ctx, void *arg)
{
    const argv_t *args = arg;
    if (args->argc == 1) {
        config_show(&ctx->lc);
        return LC_OK;
    }
    if (args->argc != 3) {
        return LC_ERR_INVALID_ARG;
    }
    return config_set(&ctx->lc, args->argv[1], args->argv[2]);
}

static int cmd_config(int argc, char **argv)
{
    argv_t args = {argc, argv};
    lc_err_t e = (lc_err_t)lock_task_call(do_config, &args);
    if (e != LC_OK) {
        printf("usage: config [<key> <value>]   keys: mode relock onetouch remote-unlock\n"
               "       remote-pin private wrong-limit lockout log log-days\n");
    }
    return argc == 1 && e == LC_OK ? 0 : report(e);
}

static int cmd_time(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: time <unix-seconds> [utc-offset-minutes]   (stand-in for Matter time sync)\n");
        return 1;
    }
    long long unix_s = 0;
    long long offset_min = 0;
    if (!parse_arg("unix-seconds", argv[1], 0, UINT32_MAX, &unix_s) ||
        (argc > 2 &&
         !parse_arg("utc-offset-minutes", argv[2], MIN_UTC_OFFSET_MIN, MAX_UTC_OFFSET_MIN, &offset_min))) {
        return report(LC_ERR_INVALID_ARG);
    }
    pe_time_set((uint32_t)unix_s, (int16_t)offset_min);
    return report(LC_OK);
}

/* ---- status / log ---------------------------------------------------------- */

static int do_status(lock_ctx_t *ctx, void *arg)
{
    lc_t *lc = &ctx->lc;
    (void)arg;
    lc_wall_time_t wall = {0};
    bool known = lc->plat.wall_time(NULL, &wall);
    printf("bolt     %s\n", bolt_name(lc->bolt));
    printf("door     %s\n", door_name(lc->door));
    printf("mode     %s\n", MODE_NAMES[lc->mode]);
    if (lc->rl.locked) {
        uint64_t now = lc->plat.now_ms(NULL);
        printf("keypad   LOCKED OUT for %llu s\n",
               (unsigned long long)((lc->rl.locked_until_ms - now) / 1000));
    } else {
        printf("keypad   ok (%u wrong)\n", lc->rl.wrong_count);
    }
    if (known) {
        printf("time     %lu (weekday %u, %02u:%02u local)\n", (unsigned long)wall.unix_s, wall.weekday,
               wall.minute_of_day / MINUTES_PER_HOUR, wall.minute_of_day % MINUTES_PER_HOUR);
    } else {
        printf("time     unknown (schedules deny until set)\n");
    }
    printf("log      %u entries\n", lc->log.count);
    printf("boot     %u, up %llu s\n", lc->boot_count, (unsigned long long)(lc->plat.now_ms(NULL) / 1000U));
    return LC_OK;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return lock_task_call(do_status, NULL);
}

/* "2026-11-07 19:56" in the clock's current local offset, or "boot 4 +11s". */
static void format_when(const lc_log_entry_t *e, char *out, size_t cap)
{
    if (e->unix_s == 0) {
        (void)snprintf(out, cap, "boot %-4u +%lus", e->boot, (unsigned long)e->uptime_s);
        return;
    }
    time_t local = (time_t)e->unix_s + ((time_t)pe_time_offset_min() * SECONDS_PER_MINUTE);
    struct tm fields;
    if (gmtime_r(&local, &fields) == NULL || strftime(out, cap, "%Y-%m-%d %H:%M:%S", &fields) == 0) {
        (void)snprintf(out, cap, "?");
    }
}

static int do_log(lock_ctx_t *ctx, void *arg)
{
    lc_t *lc = &ctx->lc;
    int n = *(int *)arg;
    int start = lc->log.count > n ? lc->log.count - n : 0;
    for (int i = start; i < lc->log.count; i++) {
        const lc_log_entry_t *e = lc_log_get(&lc->log, (uint16_t)i);
        char when[sizeof("2026-11-07 19:56:00")];
        format_when(e, when, sizeof(when));
        printf("%-19s  %-13s  %-7s  user %u\n", when,
               e->type < COUNT_OF(EVENT_NAMES) ? EVENT_NAMES[e->type] : "?",
               e->source < COUNT_OF(SOURCE_NAMES) ? SOURCE_NAMES[e->source] : "?", e->user_index);
    }
    return LC_OK;
}

static int cmd_log(int argc, char **argv)
{
    long long lines = DEFAULT_LOG_LINES;
    if (argc > 1 && !parse_arg("entries", argv[1], 1, LC_LOG_CAPACITY, &lines)) {
        return 1;
    }
    int n = (int)lines;
    return lock_task_call(do_log, &n);
}

/* ---- link pairing ---------------------------------------------------------- */

typedef struct {
    lp_role_t role;
    uint32_t seconds;
} pair_args_t;

static int do_pair(lock_ctx_t *ctx, void *arg)
{
    pair_args_t *args = arg;
    if (args->seconds == 0) {
        ll_close_pairing(&ctx->ll);
    } else {
        ll_open_pairing(&ctx->ll, args->role, args->seconds);
    }
    return LC_OK;
}

static int cmd_pair(int argc, char **argv)
{
    /* Stand-in for the physical pairing button on the lock unit. */
    if (argc < 2) {
        printf("usage: pair keypad|phone [seconds]   |   pair stop\n");
        return 1;
    }
    pair_args_t args = {.seconds = 0};
    if (strcmp(argv[1], "stop") != 0) {
        if (!parse_role(argv[1], &args.role)) {
            printf("unknown role\n");
            return 1;
        }
        long long seconds = PAIR_DEFAULT_S;
        if (argc > 2 && !parse_arg("seconds", argv[2], 1, PAIR_MAX_S, &seconds)) {
            return 1;
        }
        args.seconds = (uint32_t)seconds;
    }
    lock_task_call(do_pair, &args);
    if (args.seconds) {
        printf("pairing window open for %s, %lu s\n", argv[1], (unsigned long)args.seconds);
    } else {
        printf("pairing window closed\n");
    }
    return 0;
}

static bool peer_online(const lock_ctx_t *ctx, int peer)
{
    for (int conn = 0; conn < LL_MAX_CONNS; conn++) {
        if (ctx->ll.conns[conn].state == LL_CONN_ESTABLISHED && ctx->ll.conns[conn].peer == peer) {
            return true;
        }
    }
    return false;
}

/* What the keypad last reported: "battery 2950mV, heard boot 4 +11s". */
static void format_keypad_status(const lock_peer_status_t *status, char *out, size_t cap)
{
    char battery[sizeof("65535mV LOW")];
    if (status->battery_mv) {
        (void)snprintf(battery, sizeof(battery), "%umV%s", status->battery_mv,
                       (status->flags & LP_STATUS_LOW_BATTERY) ? " LOW" : "");
    } else {
        (void)snprintf(battery, sizeof(battery), "unknown");
    }
    (void)snprintf(out, cap, "battery %s, heard boot %u +%lus", battery, status->boot,
                   (unsigned long)status->uptime_s);
}

static int do_peers(lock_ctx_t *ctx, void *arg)
{
    int del = *(int *)arg;
    if (del >= 0) {
        if (!ll_peer_remove(&ctx->ll, (uint8_t)del)) {
            return LC_ERR_NOT_FOUND;
        }
        return LC_OK;
    }
    for (int i = 0; i < LL_MAX_PEERS; i++) {
        const ll_peer_t *p = &ctx->ll.peers.peers[i];
        if (!p->in_use) {
            continue;
        }
        char heard[sizeof("battery 65535mV LOW, heard boot 65535 +4294967295s")] = "";
        if (p->role == LP_ROLE_KEYPAD && ctx->peer_status[i].seen) {
            format_keypad_status(&ctx->peer_status[i], heard, sizeof(heard));
        }
        printf("%d  %-7s  key %02x%02x%02x%02x…  %-9s  %s\n", i,
               p->role < COUNT_OF(ROLE_NAMES) ? ROLE_NAMES[p->role] : "?", p->static_pub[0],
               p->static_pub[1], p->static_pub[2], p->static_pub[3], peer_online(ctx, i) ? "connected" : "", heard);
    }
    printf("lock key %02x%02x%02x%02x…  pairing %s\n", ctx->ll.identity.pub[0], ctx->ll.identity.pub[1],
           ctx->ll.identity.pub[2], ctx->ll.identity.pub[3], ll_pairing_open(&ctx->ll) ? "OPEN" : "closed");
    return LC_OK;
}

static int cmd_peers(int argc, char **argv)
{
    int del = -1;
    if (argc == 3 && strcmp(argv[1], "del") == 0) {
        long long index = 0;
        if (!parse_arg("peer index", argv[2], 0, LL_MAX_PEERS - 1, &index)) {
            return 1;
        }
        del = (int)index;
    } else if (argc != 1) {
        printf("usage: peers [del <index>]\n");
        return 1;
    }
    lc_err_t e = (lc_err_t)lock_task_call(do_peers, &del);
    return del >= 0 ? report(e) : 0;
}

/* ---- simulation ------------------------------------------------------------ */

static int cmd_sim(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "door") == 0 &&
        (strcmp(argv[2], "open") == 0 || strcmp(argv[2], "closed") == 0)) {
        lock_task_post_door(strcmp(argv[2], "open") == 0 ? LC_DOOR_OPEN : LC_DOOR_CLOSED);
    } else if (argc == 3 && strcmp(argv[1], "turn") == 0 &&
               (strcmp(argv[2], "locked") == 0 || strcmp(argv[2], "unlocked") == 0)) {
        lock_task_post_bolt_position(strcmp(argv[2], "locked") == 0 ? LC_BOLT_LOCKED : LC_BOLT_UNLOCKED);
    } else if (argc == 2 && strcmp(argv[1], "jam") == 0) {
        motor_sim_fail_next();
    } else {
        printf("usage: sim door open|closed | sim turn locked|unlocked | sim jam\n");
        return 1;
    }
    return 0;
}

static int cmd_factory_reset(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "confirm") != 0) {
        printf("erases all users, codes, log, settings and paired peers. usage: factory-reset confirm\n");
        return 1;
    }
    ESP_ERROR_CHECK(pe_store_erase());
    printf("erased; restarting\n");
    esp_restart();
    return 0;
}

/* ---- registration ---------------------------------------------------------- */

static void reg(const char *name, const char *help, esp_console_cmd_func_t func)
{
    const esp_console_cmd_t cmd = {.command = name, .help = help, .func = func};
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "lochness>";
    repl_config.task_stack_size = CONSOLE_STACK;

    ESP_ERROR_CHECK(esp_console_register_help_command());
    reg("pin", "Keypad: enter a PIN to unlock", cmd_pin);
    reg("lock", "Keypad: lock (one-touch, or with optional PIN)", cmd_lock);
    reg("remote", "Matter stand-in: remote lock|unlock [pin]", cmd_remote);
    reg("app", "App: lock|unlock", cmd_app);
    reg("user", "Manage users", cmd_user);
    reg("code", "Manage PIN codes", cmd_code);
    reg("sched", "Week-day schedules for schedule users", cmd_sched);
    reg("config", "Show or change settings", cmd_config);
    reg("time", "Set wall-clock time (Matter time sync stand-in)", cmd_time);
    reg("status", "Show lock state", cmd_status);
    reg("log", "Show the last N access log entries", cmd_log);
    reg("pair", "Open a pairing window (stand-in for the lock's pair button)", cmd_pair);
    reg("peers", "List or remove paired keypads/phones", cmd_peers);
    reg("sim", "Simulate door sensor, manual turns, jams", cmd_sim);
    reg("factory-reset", "Erase all lock state", cmd_factory_reset);

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#else
#error Unsupported console type
#endif
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
