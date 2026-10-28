#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_system.h"
#include "lock_task.h"
#include "motor_sim.h"
#include "platform_esp/pe_platform.h"

#define PIN_ARG(s) (const uint8_t *)(s), strlen(s)

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
    return (unsigned)e < sizeof(names) / sizeof(names[0]) ? names[e] : "?";
}

static const char *bolt_name(lc_bolt_t b)
{
    static const char *names[] = {"unknown", "locked", "unlocked", "locking", "unlocking", "JAMMED"};
    return (unsigned)b < 6 ? names[b] : "?";
}

static const char *door_name(lc_door_t d)
{
    static const char *names[] = {"unknown", "closed", "open"};
    return (unsigned)d < 3 ? names[d] : "?";
}

static const char *mode_names[] = {"normal", "vacation", "privacy", "noremote", "passage"};
static const char *user_type_names[] = {"unrestricted", "schedule", "disposable", "nonaccess"};
static const char *source_names[] = {"keypad", "remote", "app", "manual", "auto"};
static const char *event_names[] = {"locked",        "unlocked", "wrong-code",  "lockout",
                                    "access-denied", "jammed",   "door-opened", "door-closed"};

static int lookup(const char *s, const char **names, int n)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(s, names[i]) == 0) {
            return i;
        }
    }
    return -1;
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
    request_args_t *a = arg;
    uint16_t user = 0;
    lc_err_t e = lc_request(lc, a->op, a->source, a->pin ? (const uint8_t *)a->pin : NULL,
                            a->pin ? strlen(a->pin) : 0, &user);
    if (e == LC_OK && user) {
        printf("user %u: ", user);
    }
    return e;
}

static int request(lc_op_t op, lc_source_t source, const char *pin)
{
    request_args_t a = {.op = op, .source = source, .pin = pin};
    return report((lc_err_t)lock_task_call(do_request, &a));
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

static int cmd_remote(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "lock") && strcmp(argv[1], "unlock"))) {
        printf("usage: remote lock|unlock [pin]\n");
        return 1;
    }
    return request(strcmp(argv[1], "lock") == 0 ? LC_OP_LOCK : LC_OP_UNLOCK, LC_SOURCE_REMOTE,
                   argc > 2 ? argv[2] : NULL);
}

static int cmd_app(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "lock") && strcmp(argv[1], "unlock"))) {
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

static int do_user(lock_ctx_t *ctx, void *arg)
{
    lc_t *lc = &ctx->lc;
    argv_t *a = arg;
    const char *sub = a->argv[1];

    if (strcmp(sub, "list") == 0) {
        for (uint16_t i = 1; i <= LC_MAX_USERS; i++) {
            const lc_user_t *u = lc_user_get(&lc->creds, i);
            if (!u) {
                continue;
            }
            int codes = 0;
            for (int c = 0; c < LC_MAX_CREDENTIALS; c++) {
                codes += lc->creds.creds[c].in_use && lc->creds.creds[c].user_index == i;
            }
            printf("%2u  %-10s  %-12s  %s  codes=%d  schedules=%u\n", i, u->name,
                   user_type_names[u->type], u->enabled ? "enabled " : "disabled", codes,
                   u->schedule_count);
        }
        return LC_OK;
    }
    if (strcmp(sub, "add") == 0 && a->argc >= 3) {
        int type = a->argc >= 4 ? lookup(a->argv[3], user_type_names, 4) : LC_USER_UNRESTRICTED;
        if (type < 0) {
            return LC_ERR_INVALID_ARG;
        }
        uint16_t idx = lc_user_next_free(&lc->creds);
        if (idx == LC_INDEX_NONE) {
            return LC_ERR_FULL;
        }
        lc_err_t e = lc_user_set(&lc->creds, idx, (lc_user_type_t)type, a->argv[2]);
        if (e == LC_OK) {
            lc->hooks.persist(lc->hooks.ctx, LC_PERSIST_CREDS);
            printf("user %u: ", idx);
        }
        return e;
    }
    if (a->argc != 3) {
        return LC_ERR_INVALID_ARG;
    }
    uint16_t idx = (uint16_t)atoi(a->argv[2]);
    lc_err_t e;
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

static int cmd_user(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: user list | add <name> [unrestricted|schedule|disposable|nonaccess]\n"
               "       user del|enable|disable <index>\n");
        return 1;
    }
    argv_t a = {argc, argv};
    lc_err_t e = (lc_err_t)lock_task_call(do_user, &a);
    return strcmp(argv[1], "list") == 0 && e == LC_OK ? 0 : report(e);
}

static int do_code(lock_ctx_t *ctx, void *arg)
{
    lc_t *lc = &ctx->lc;
    argv_t *a = arg;
    lc_err_t e;
    if (strcmp(a->argv[1], "add") == 0 && a->argc == 4) {
        uint16_t cred = lc_cred_next_free(&lc->creds);
        if (cred == LC_INDEX_NONE) {
            return LC_ERR_FULL;
        }
        e = lc_pin_set(&lc->creds, &lc->plat, cred, (uint16_t)atoi(a->argv[2]), PIN_ARG(a->argv[3]));
        if (e == LC_OK) {
            printf("code %u: ", cred);
        }
    } else if (strcmp(a->argv[1], "del") == 0 && a->argc == 3) {
        e = lc_pin_clear(&lc->creds, (uint16_t)atoi(a->argv[2]));
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
    argv_t a = {argc, argv};
    return report((lc_err_t)lock_task_call(do_code, &a));
}

static bool parse_hhmm(const char *s, uint16_t *out)
{
    int h, m;
    if (sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 24 || m < 0 || m > 59 || h * 60 + m > 1440) {
        return false;
    }
    *out = (uint16_t)(h * 60 + m);
    return true;
}

static int do_sched(lock_ctx_t *ctx, void *arg)
{
    lc_t *lc = &ctx->lc;
    argv_t *a = arg;
    uint16_t user = (uint16_t)atoi(a->argv[1]);
    if (a->argc == 3 && strcmp(a->argv[2], "clear") == 0) {
        lc_err_t e = lc_user_set_schedules(&lc->creds, user, NULL, 0);
        if (e == LC_OK) {
            lc->hooks.persist(lc->hooks.ctx, LC_PERSIST_CREDS);
        }
        return e;
    }
    lc_weekday_schedule_t s = {0};
    for (const char *d = a->argv[2]; *d; d++) {
        if (*d < '0' || *d > '6') {
            return LC_ERR_INVALID_ARG;
        }
        s.days_mask |= (uint8_t)(1u << (*d - '0'));
    }
    if (!parse_hhmm(a->argv[3], &s.start_min) || !parse_hhmm(a->argv[4], &s.end_min)) {
        return LC_ERR_INVALID_ARG;
    }
    const lc_user_t *u = lc_user_get(&lc->creds, user);
    if (!u) {
        return LC_ERR_NOT_FOUND;
    }
    if (u->schedule_count >= LC_MAX_WEEKDAY_SCHEDULES) {
        return LC_ERR_FULL;
    }
    lc_weekday_schedule_t all[LC_MAX_WEEKDAY_SCHEDULES];
    memcpy(all, u->schedules, sizeof(all));
    uint8_t n = u->schedule_count;
    all[n++] = s;
    lc_err_t e = lc_user_set_schedules(&lc->creds, user, all, n);
    if (e == LC_OK) {
        lc->hooks.persist(lc->hooks.ctx, LC_PERSIST_CREDS);
    }
    return e;
}

static int cmd_sched(int argc, char **argv)
{
    if (!(argc == 5 || (argc == 3 && strcmp(argv[2], "clear") == 0))) {
        printf("usage: sched <user> <days> <HH:MM> <HH:MM>   days: 0=Sun..6=Sat, e.g. 12345\n"
               "       sched <user> clear\n");
        return 1;
    }
    argv_t a = {argc, argv};
    return report((lc_err_t)lock_task_call(do_sched, &a));
}

/* ---- config ---------------------------------------------------------------- */

static bool parse_bool(const char *s, bool *out)
{
    if (!strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "true")) {
        *out = true;
        return true;
    }
    if (!strcmp(s, "off") || !strcmp(s, "0") || !strcmp(s, "false")) {
        *out = false;
        return true;
    }
    return false;
}

static int do_config(lock_ctx_t *ctx, void *arg)
{
    lc_t *lc = &ctx->lc;
    argv_t *a = arg;
    lc_config_t *c = &lc->cfg;
    if (a->argc == 1) {
        printf("mode            %s\n", mode_names[lc->mode]);
        printf("relock          %lu s (0 = off)\n", (unsigned long)c->auto_relock_s);
        printf("onetouch        %s\n", c->one_touch_lock ? "on" : "off");
        printf("remote-unlock   %s\n", c->remote_unlock_allowed ? "on" : "off");
        printf("remote-pin      %s\n", c->require_pin_for_remote ? "on" : "off");
        printf("private         %s\n", c->private_events ? "on" : "off");
        printf("wrong-limit     %u\n", c->ratelimit.limit);
        printf("lockout         %lu s (max %lu s)\n", (unsigned long)c->ratelimit.base_lockout_s,
               (unsigned long)c->ratelimit.max_lockout_s);
        printf("log             %s, %u days\n", c->log.enabled ? "on" : "off", c->log.max_age_days);
        return LC_OK;
    }
    if (a->argc != 3) {
        return LC_ERR_INVALID_ARG;
    }
    const char *k = a->argv[1], *v = a->argv[2];
    bool b;
    if (!strcmp(k, "mode")) {
        int m = lookup(v, mode_names, 5);
        if (m < 0) {
            return LC_ERR_INVALID_ARG;
        }
        lc_set_mode(lc, (lc_mode_t)m);
        return LC_OK; /* TODO: persist mode with the rest of the runtime config */
    } else if (!strcmp(k, "relock")) {
        c->auto_relock_s = (uint32_t)atoi(v);
    } else if (!strcmp(k, "onetouch") && parse_bool(v, &b)) {
        c->one_touch_lock = b;
    } else if (!strcmp(k, "remote-unlock") && parse_bool(v, &b)) {
        c->remote_unlock_allowed = b;
    } else if (!strcmp(k, "remote-pin") && parse_bool(v, &b)) {
        c->require_pin_for_remote = b;
    } else if (!strcmp(k, "private") && parse_bool(v, &b)) {
        c->private_events = b;
    } else if (!strcmp(k, "wrong-limit") && atoi(v) >= 1 && atoi(v) <= 255) {
        c->ratelimit.limit = (uint8_t)atoi(v);
    } else if (!strcmp(k, "lockout") && atoi(v) >= 1) {
        c->ratelimit.base_lockout_s = (uint32_t)atoi(v);
    } else if (!strcmp(k, "log") && parse_bool(v, &b)) {
        c->log.enabled = b;
        if (!b) {
            lc_log_prune(&lc->log, &c->log, 0); /* disabling erases history */
            lc->hooks.persist(lc->hooks.ctx, LC_PERSIST_LOG);
        }
    } else if (!strcmp(k, "log-days")) {
        c->log.max_age_days = (uint16_t)atoi(v);
    } else {
        return LC_ERR_INVALID_ARG;
    }
    lock_task_save_config(lc);
    return LC_OK;
}

static int cmd_config(int argc, char **argv)
{
    argv_t a = {argc, argv};
    lc_err_t e = (lc_err_t)lock_task_call(do_config, &a);
    if (e != LC_OK) {
        printf("usage: config [<key> <value>]   keys: mode relock onetouch remote-unlock\n"
               "       remote-pin private wrong-limit lockout log log-days\n");
    }
    return argc == 1 && e == LC_OK ? 0 : report(e);
}

static int cmd_time(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: time <unix-seconds> [utc-offset-minutes]   (stand-in for hub time sync)\n");
        return 1;
    }
    pe_time_set((uint32_t)strtoul(argv[1], NULL, 10), (int16_t)(argc > 2 ? atoi(argv[2]) : 0));
    return report(LC_OK);
}

/* ---- status / log ---------------------------------------------------------- */

static int do_status(lock_ctx_t *ctx, void *arg)
{
    lc_t *lc = &ctx->lc;
    (void)arg;
    lc_wall_time_t wt;
    bool known = lc->plat.wall_time(NULL, &wt);
    printf("bolt     %s\n", bolt_name(lc->bolt));
    printf("door     %s\n", door_name(lc->door));
    printf("mode     %s\n", mode_names[lc->mode]);
    if (lc->rl.locked) {
        uint64_t now = lc->plat.now_ms(NULL);
        printf("keypad   LOCKED OUT for %llu s\n",
               (unsigned long long)((lc->rl.locked_until_ms - now) / 1000));
    } else {
        printf("keypad   ok (%u wrong)\n", lc->rl.wrong_count);
    }
    if (known) {
        printf("time     %lu (weekday %u, %02u:%02u local)\n", (unsigned long)wt.unix_s, wt.weekday,
               wt.minute_of_day / 60, wt.minute_of_day % 60);
    } else {
        printf("time     unknown (schedules deny until set)\n");
    }
    printf("log      %u entries\n", lc->log.count);
    return LC_OK;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return lock_task_call(do_status, NULL);
}

static int do_log(lock_ctx_t *ctx, void *arg)
{
    lc_t *lc = &ctx->lc;
    int n = *(int *)arg;
    int start = lc->log.count > n ? lc->log.count - n : 0;
    for (int i = start; i < lc->log.count; i++) {
        const lc_log_entry_t *e = lc_log_get(&lc->log, (uint16_t)i);
        printf("%10lu  up+%-7lu  %-13s  %-7s  user %u\n", (unsigned long)e->unix_s,
               (unsigned long)e->uptime_s, e->type < 8 ? event_names[e->type] : "?",
               e->source < 5 ? source_names[e->source] : "?", e->user_index);
    }
    return LC_OK;
}

static int cmd_log(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 20;
    return lock_task_call(do_log, &n);
}

/* ---- link pairing ---------------------------------------------------------- */

static const char *role_names[] = {"?", "keypad", "hub", "phone"};

typedef struct {
    lp_role_t role;
    uint32_t seconds;
} pair_args_t;

static int do_pair(lock_ctx_t *ctx, void *arg)
{
    pair_args_t *a = arg;
    if (a->seconds == 0) {
        ll_close_pairing(&ctx->ll);
    } else {
        ll_open_pairing(&ctx->ll, a->role, a->seconds);
    }
    return LC_OK;
}

static int cmd_pair(int argc, char **argv)
{
    /* Stand-in for the physical pairing button on the lock unit. */
    if (argc < 2) {
        printf("usage: pair keypad|hub|phone [seconds]   |   pair stop\n");
        return 1;
    }
    pair_args_t a = {.seconds = argc > 2 ? (uint32_t)atoi(argv[2]) : 60};
    if (!strcmp(argv[1], "stop")) {
        a.seconds = 0;
    } else {
        int r = lookup(argv[1], role_names, 4);
        if (r < 1) {
            printf("unknown role\n");
            return 1;
        }
        a.role = (lp_role_t)r;
    }
    lock_task_call(do_pair, &a);
    if (a.seconds) {
        printf("pairing window open for %s, %lu s\n", argv[1], (unsigned long)a.seconds);
    } else {
        printf("pairing window closed\n");
    }
    return 0;
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
        bool online = false;
        for (int c = 0; c < LL_MAX_CONNS; c++) {
            online |= ctx->ll.conns[c].state == LL_CONN_ESTABLISHED && ctx->ll.conns[c].peer == i;
        }
        printf("%d  %-7s  key %02x%02x%02x%02x…  %s\n", i, p->role <= 3 ? role_names[p->role] : "?",
               p->static_pub[0], p->static_pub[1], p->static_pub[2], p->static_pub[3],
               online ? "connected" : "");
    }
    printf("lock key %02x%02x%02x%02x…  pairing %s\n", ctx->ll.identity.pub[0], ctx->ll.identity.pub[1],
           ctx->ll.identity.pub[2], ctx->ll.identity.pub[3], ll_pairing_open(&ctx->ll) ? "OPEN" : "closed");
    return LC_OK;
}

static int cmd_peers(int argc, char **argv)
{
    int del = -1;
    if (argc == 3 && !strcmp(argv[1], "del")) {
        del = atoi(argv[2]);
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
    if (argc == 3 && !strcmp(argv[1], "door") && (!strcmp(argv[2], "open") || !strcmp(argv[2], "closed"))) {
        lock_task_post_door(!strcmp(argv[2], "open") ? LC_DOOR_OPEN : LC_DOOR_CLOSED);
    } else if (argc == 3 && !strcmp(argv[1], "turn") &&
               (!strcmp(argv[2], "locked") || !strcmp(argv[2], "unlocked"))) {
        lock_task_post_bolt_position(!strcmp(argv[2], "locked") ? LC_BOLT_LOCKED : LC_BOLT_UNLOCKED);
    } else if (argc == 2 && !strcmp(argv[1], "jam")) {
        motor_sim_fail_next();
    } else {
        printf("usage: sim door open|closed | sim turn locked|unlocked | sim jam\n");
        return 1;
    }
    return 0;
}

static int cmd_factory_reset(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "confirm")) {
        printf("erases all users, codes, log, settings and paired peers. usage: factory-reset confirm\n");
        return 1;
    }
    ESP_ERROR_CHECK(pe_store_erase());
    printf("erased; restarting\n");
    esp_restart();
    return 0;
}

/* ---- registration ---------------------------------------------------------- */

static void reg(const char *name, const char *help, esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t cmd = {.command = name, .help = help, .func = fn};
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "lochness>";
    repl_config.task_stack_size = 6144;

    ESP_ERROR_CHECK(esp_console_register_help_command());
    reg("pin", "Keypad: enter a PIN to unlock", cmd_pin);
    reg("lock", "Keypad: lock (one-touch, or with optional PIN)", cmd_lock);
    reg("remote", "Hub: remote lock|unlock [pin]", cmd_remote);
    reg("app", "App: lock|unlock", cmd_app);
    reg("user", "Manage users", cmd_user);
    reg("code", "Manage PIN codes", cmd_code);
    reg("sched", "Week-day schedules for schedule users", cmd_sched);
    reg("config", "Show or change settings", cmd_config);
    reg("time", "Set wall-clock time (hub stand-in)", cmd_time);
    reg("status", "Show lock state", cmd_status);
    reg("log", "Show the last N access log entries", cmd_log);
    reg("pair", "Open a pairing window (stand-in for the lock's pair button)", cmd_pair);
    reg("peers", "List or remove paired keypads/hubs/phones", cmd_peers);
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
