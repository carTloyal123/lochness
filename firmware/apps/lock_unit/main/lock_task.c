#include "lock_task.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ble_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "link_proto/lp.h"
#include "ln_store.h"
#include "lock_bridge.h"
#include "lock_core/lc_lock.h"
#include "lock_core/lc_log.h"
#include "lock_link/ll.h"
#include "motor_sim.h"
#include "platform_esp/pe_platform.h"
#include "secure_link/sl_crypto.h"
#include "status_led.h"

static const char *const TAG = "lock";

enum {
    /* The log is not critical-path; batch its flash writes. */
    LOG_FLUSH_DELAY_MS = 30000,
    /* Events raised while a frame is being handled, sent once it is done. */
    STATE_QUEUE_LEN = 8,
    /* Longest single wait; the loop simply re-arms after it. */
    MAX_WAIT_MS = 3600000,
    /* Noise handshakes (X25519) and PIN matching (HMAC per slot) need room. */
    LOCK_TASK_STACK = 10240,
    LOCK_TASK_PRIORITY = 5,
};

typedef enum {
    EV_CALL,
    EV_MOTOR_DONE,
    EV_DOOR,
    EV_BOLT_POSITION,
    EV_LINK_CONNECT,
    EV_LINK_DISCONNECT,
    EV_LINK_FRAME,
} ev_kind_t;

typedef struct {
    ev_kind_t kind;
    union {
        struct {
            lock_task_fn_t fn;
            void *arg;
            int *ret;
            TaskHandle_t waiter;
        } call;
        bool motor_ok;
        lc_door_t door;
        lc_bolt_t bolt;
        struct {
            uint16_t conn;
            uint16_t len;
            uint8_t data[LP_MAX_FRAME];
        } link;
    };
} ev_t;

/* Owned by the lock task; other tasks only touch `queue`. */
static struct {
    lock_ctx_t ctx;
    QueueHandle_t queue;
    bool log_dirty;
    uint64_t log_dirty_since_ms;
    bool adv_pairing;
    /* LOCK_STATE messages waiting for the current handler to return, so
     * lock_link is never re-entered from inside its own callbacks. */
    lp_msg_t state_q[STATE_QUEUE_LEN];
    uint8_t state_q_len;
    /* Last state pushed to phones, to detect changes without an event. */
    uint8_t sent_bolt;
    uint8_t sent_door;
} s_lock; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): module state

static uint64_t now_ms(void) { return s_lock.ctx.lc.plat.now_ms(NULL); }

/* ---- lock_core hooks ------------------------------------------------------- */

static const char *event_name(lc_event_type_t t)
{
    switch (t) {
    case LC_EVENT_LOCKED: return "locked";
    case LC_EVENT_UNLOCKED: return "unlocked";
    case LC_EVENT_WRONG_CODE: return "wrong code";
    case LC_EVENT_LOCKOUT_STARTED: return "lockout started";
    case LC_EVENT_ACCESS_DENIED: return "access denied";
    case LC_EVENT_JAMMED: return "JAMMED";
    case LC_EVENT_DOOR_OPENED: return "door opened";
    case LC_EVENT_DOOR_CLOSED: return "door closed";
    }
    return "?";
}

static void queue_state(const lc_event_t *ev)
{
    if (s_lock.state_q_len == STATE_QUEUE_LEN) {
        ESP_LOGW(TAG, "state queue full; phones miss an event");
        return;
    }
    lock_bridge_state(&s_lock.ctx.lc, ev, &s_lock.state_q[s_lock.state_q_len++]);
}

static void flush_state(void)
{
    const lc_t *lc = &s_lock.ctx.lc;
    lp_msg_t now;
    lock_bridge_state(lc, NULL, &now);
    if (s_lock.state_q_len == 0 &&
        (now.lock_state.bolt != s_lock.sent_bolt || now.lock_state.door != s_lock.sent_door)) {
        s_lock.state_q[s_lock.state_q_len++] = now; /* e.g. motor started: no event, new state */
    }
    for (uint8_t i = 0; i < s_lock.state_q_len; i++) {
        ll_broadcast_state(&s_lock.ctx.ll, &s_lock.state_q[i]);
    }
    s_lock.state_q_len = 0;
    s_lock.sent_bolt = now.lock_state.bolt;
    s_lock.sent_door = now.lock_state.door;
}

static void hook_notify(void *ctx, const lc_event_t *ev)
{
    (void)ctx;
    ESP_LOGI(TAG, "event: %s (source %d, user %u)", event_name(ev->type), ev->source, ev->user_index);
    queue_state(ev);
}

static void hook_persist(void *ctx, lc_persist_t what)
{
    (void)ctx;
    if (what == LC_PERSIST_LOG) {
        if (!s_lock.log_dirty) {
            s_lock.log_dirty = true;
            s_lock.log_dirty_since_ms = now_ms();
        }
        return;
    }
    pe_store_save(&s_lock.ctx.lc, what);
}

void lock_task_save_config(const lc_t *lc) { pe_store_save_config(&lc->cfg); }
void lock_task_save_peers(const ll_t *ll) { pe_store_save_peers(&ll->peers, sizeof(ll->peers)); }

/* ---- lock_link hooks ------------------------------------------------------- */

static uint64_t link_now(void *ctx) { (void)ctx; return now_ms(); }

static bool link_random(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len); /* radio is on, so the RNG is entropy-backed */
    return true;
}

static bool link_send(void *ctx, uint16_t conn, const uint8_t *frame, size_t len)
{
    (void)ctx;
    return ble_server_send(conn, frame, len);
}

static void link_disconnect(void *ctx, uint16_t conn)
{
    (void)ctx;
    ble_server_disconnect(conn);
}

static void link_peers_changed(void *ctx)
{
    (void)ctx;
    lock_task_save_peers(&s_lock.ctx.ll);
}

static ll_access_result_t link_access(void *ctx, lp_role_t role, lp_op_t op, const uint8_t *pin, size_t len)
{
    (void)ctx;
    return lock_bridge_access(&s_lock.ctx.lc, role, op, pin, len);
}

static lp_bolt_t link_bolt(void *ctx)
{
    (void)ctx;
    return lock_bridge_bolt(&s_lock.ctx.lc);
}

static void link_keypad_status(void *ctx, uint8_t peer, uint16_t battery_mv, uint8_t flags)
{
    (void)ctx;
    if (peer >= LL_MAX_PEERS) {
        return;
    }
    lock_peer_status_t *status = &s_lock.ctx.peer_status[peer];
    bool changed = !status->seen || status->battery_mv != battery_mv || status->flags != flags;
    status->seen = true;
    status->battery_mv = battery_mv;
    status->flags = flags;
    status->boot = s_lock.ctx.lc.boot_count;
    status->uptime_s = (uint32_t)(now_ms() / 1000U);
    /* TODO: surface low battery / tamper as lock events (Matter alarms). */
    if (!changed) {
        return;
    }
    if (battery_mv == 0) {
        ESP_LOGI(TAG, "keypad %u: battery unknown (no sense circuit), flags 0x%02x", peer, flags);
    } else {
        ESP_LOGI(TAG, "keypad %u: battery %u mV%s, flags 0x%02x", peer, battery_mv,
                 (flags & LP_STATUS_LOW_BATTERY) ? " LOW" : "", flags);
    }
}

static void link_time_set(void *ctx, uint32_t unix_s, int16_t utc_offset_min)
{
    (void)ctx;
    bool first = !pe_time_known();
    pe_time_set(unix_s, utc_offset_min);
    if (first) {
        ESP_LOGI(TAG, "clock set by phone: %lu, UTC%+d min", (unsigned long)unix_s, utc_offset_min);
    }
}

static void link_snapshot(void *ctx, lp_msg_t *state)
{
    (void)ctx;
    lock_bridge_state(&s_lock.ctx.lc, NULL, state);
}

static void link_paired(void *ctx, uint8_t peer, lp_role_t role, const uint8_t hash[SL_HASH_LEN])
{
    (void)ctx;
    /* The handshake hash is the basis for an out-of-band confirmation code
     * (shown in the app) once phones can approve pairings. */
    ESP_LOGI(TAG, "paired peer %u (role %d), code %02x%02x%02x", peer, role, hash[0], hash[1], hash[2]);
}

/* ---- task ------------------------------------------------------------------ */

static TickType_t ticks_until(uint64_t deadline_ms, uint64_t now)
{
    if (deadline_ms == UINT64_MAX) {
        return portMAX_DELAY;
    }
    if (deadline_ms <= now) {
        return 0;
    }
    uint64_t ms = deadline_ms - now;
    return ms > MAX_WAIT_MS ? pdMS_TO_TICKS(MAX_WAIT_MS) : pdMS_TO_TICKS((uint32_t)ms) + 1;
}

static uint64_t min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }

static void handle(const ev_t *ev)
{
    switch (ev->kind) {
    case EV_CALL:
        *ev->call.ret = ev->call.fn(&s_lock.ctx, ev->call.arg);
        xTaskNotifyGive(ev->call.waiter);
        break;
    case EV_MOTOR_DONE:
        lc_on_motor_done(&s_lock.ctx.lc, ev->motor_ok);
        break;
    case EV_DOOR:
        lc_on_door(&s_lock.ctx.lc, ev->door);
        break;
    case EV_BOLT_POSITION:
        lc_on_bolt_position(&s_lock.ctx.lc, ev->bolt);
        break;
    case EV_LINK_CONNECT:
        ll_on_connect(&s_lock.ctx.ll, ev->link.conn);
        break;
    case EV_LINK_DISCONNECT:
        ll_on_disconnect(&s_lock.ctx.ll, ev->link.conn);
        break;
    case EV_LINK_FRAME:
        ll_on_frame(&s_lock.ctx.ll, ev->link.conn, ev->link.data, ev->link.len);
        break;
    }
}

static void lock_task(void *arg)
{
    (void)arg;
    static ev_t ev; /* large (frame buffer); keep off the stack */
    for (;;) {
        uint64_t now = now_ms();
        uint64_t deadline = min_u64(lc_next_deadline_ms(&s_lock.ctx.lc), ll_next_deadline_ms(&s_lock.ctx.ll));
        if (s_lock.log_dirty) {
            deadline = min_u64(deadline, s_lock.log_dirty_since_ms + LOG_FLUSH_DELAY_MS);
        }

        if (xQueueReceive(s_lock.queue, &ev, ticks_until(deadline, now)) == pdTRUE) {
            handle(&ev);
        }

        lc_tick(&s_lock.ctx.lc);
        ll_tick(&s_lock.ctx.ll);

        flush_state();

        bool pairing = ll_pairing_open(&s_lock.ctx.ll);
        if (pairing != s_lock.adv_pairing) {
            s_lock.adv_pairing = pairing;
            ble_server_set_pairing(pairing);
            ESP_LOGI(TAG, "pairing window %s", pairing ? "open" : "closed");
        }
        status_led_show(lock_bridge_bolt(&s_lock.ctx.lc), pairing);
        if (s_lock.log_dirty && now_ms() >= s_lock.log_dirty_since_ms + LOG_FLUSH_DELAY_MS) {
            pe_store_save(&s_lock.ctx.lc, LC_PERSIST_LOG);
            s_lock.log_dirty = false;
        }
    }
}

void lock_task_start(void)
{
    lc_platform_t plat = pe_platform();
    lc_hooks_t hooks = {
        .motor_start = motor_sim_start,
        .motor_stop = motor_sim_stop,
        .notify = hook_notify,
        .persist = hook_persist,
    };
    lc_config_t cfg = lc_config_default();
    pe_store_load_config(&cfg);

    lc_init(&s_lock.ctx.lc, &plat, &hooks, &cfg);
    s_lock.ctx.lc.boot_count = pe_boot_count();
    ESP_ERROR_CHECK(pe_store_load(&s_lock.ctx.lc));
    /* TODO(M1): read the real bolt position from the encoder. */
    lc_boot(&s_lock.ctx.lc, LC_BOLT_LOCKED, LC_DOOR_UNKNOWN);

    /* Long-term link identity. Must be created before the radio starts. */
    sl_keypair_t identity;
    ESP_ERROR_CHECK(ln_secret_get_or_create("link_id", identity.priv, sizeof(identity.priv)));
    sl_keypair_from_private(&identity);
    ll_hooks_t link_hooks = {
        .now_ms = link_now,
        .random = link_random,
        .send = link_send,
        .disconnect = link_disconnect,
        .peers_changed = link_peers_changed,
        .access = link_access,
        .bolt = link_bolt,
        .keypad_status = link_keypad_status,
        .paired = link_paired,
        .time_set = link_time_set,
        .snapshot = link_snapshot,
    };
    ll_init(&s_lock.ctx.ll, &identity, &link_hooks);
    sl_wipe(&identity, sizeof(identity));
    pe_store_load_peers(&s_lock.ctx.ll.peers, sizeof(s_lock.ctx.ll.peers));
    ESP_LOGI(TAG, "boot %u, %u paired peer(s); lock state %u bytes", s_lock.ctx.lc.boot_count,
             ll_peer_count(&s_lock.ctx.ll), (unsigned)sizeof(s_lock.ctx));
    lp_msg_t initial;
    lock_bridge_state(&s_lock.ctx.lc, NULL, &initial);
    s_lock.sent_bolt = initial.lock_state.bolt;
    s_lock.sent_door = initial.lock_state.door;

    s_lock.queue = xQueueCreate(16, sizeof(ev_t));
    configASSERT(s_lock.queue);
    xTaskCreate(lock_task, "lock", LOCK_TASK_STACK, NULL, LOCK_TASK_PRIORITY, NULL);
}

int lock_task_call(lock_task_fn_t func, void *arg)
{
    int ret = -1;
    ev_t *ev = calloc(1, sizeof(*ev));
    configASSERT(ev);
    ev->kind = EV_CALL;
    ev->call.fn = func;
    ev->call.arg = arg;
    ev->call.ret = &ret;
    ev->call.waiter = xTaskGetCurrentTaskHandle();
    xQueueSend(s_lock.queue, ev, portMAX_DELAY);
    free(ev);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return ret;
}

static void post(ev_t *ev)
{
    if (xQueueSend(s_lock.queue, ev, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "event queue full; dropped event %d", ev->kind);
    }
}

void lock_task_post_motor_done(bool success)
{
    ev_t ev = {.kind = EV_MOTOR_DONE, .motor_ok = success};
    post(&ev);
}

void lock_task_post_door(lc_door_t door)
{
    ev_t ev = {.kind = EV_DOOR, .door = door};
    post(&ev);
}

void lock_task_post_bolt_position(lc_bolt_t bolt)
{
    ev_t ev = {.kind = EV_BOLT_POSITION, .bolt = bolt};
    post(&ev);
}

void lock_task_post_link_connect(uint16_t conn)
{
    ev_t ev = {.kind = EV_LINK_CONNECT, .link = {.conn = conn}};
    post(&ev);
}

void lock_task_post_link_disconnect(uint16_t conn)
{
    ev_t ev = {.kind = EV_LINK_DISCONNECT, .link = {.conn = conn}};
    post(&ev);
}

void lock_task_post_link_frame(uint16_t conn, const uint8_t *frame, size_t len)
{
    if (len > LP_MAX_FRAME) {
        return;
    }
    static ev_t ev; /* only called from the NimBLE host task */
    ev.kind = EV_LINK_FRAME;
    ev.link.conn = conn;
    ev.link.len = (uint16_t)len;
    memcpy(ev.link.data, frame, len);
    post(&ev);
}
