#include "lock_task.h"

#include <stdlib.h>
#include <string.h>

#include "ble_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ln_store.h"
#include "lock_bridge.h"
#include "motor_sim.h"
#include "platform_esp/pe_platform.h"

static const char *TAG = "lock";

/* The log is not critical-path; batch its flash writes. */
#define LOG_FLUSH_DELAY_MS 30000

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

static lock_ctx_t s_ctx;
static QueueHandle_t s_queue;
static bool s_log_dirty;
static uint64_t s_log_dirty_since_ms;
static bool s_adv_pairing;

static uint64_t now_ms(void) { return s_ctx.lc.plat.now_ms(NULL); }

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

static void hook_notify(void *ctx, const lc_event_t *ev)
{
    (void)ctx;
    /* TODO(M3): forward to the hub and connected phones over the link. */
    ESP_LOGI(TAG, "event: %s (source %d, user %u)", event_name(ev->type), ev->source, ev->user_index);
}

static void hook_persist(void *ctx, lc_persist_t what)
{
    (void)ctx;
    if (what == LC_PERSIST_LOG) {
        if (!s_log_dirty) {
            s_log_dirty = true;
            s_log_dirty_since_ms = now_ms();
        }
        return;
    }
    pe_store_save(&s_ctx.lc, what);
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
    lock_task_save_peers(&s_ctx.ll);
}

static ll_access_result_t link_access(void *ctx, lp_role_t role, lp_op_t op, const uint8_t *pin, size_t len)
{
    (void)ctx;
    return lock_bridge_access(&s_ctx.lc, role, op, pin, len);
}

static lp_bolt_t link_bolt(void *ctx)
{
    (void)ctx;
    return lock_bridge_bolt(&s_ctx.lc);
}

static void link_keypad_status(void *ctx, uint8_t peer, uint16_t battery_mv, uint8_t flags)
{
    (void)ctx;
    /* TODO: surface low battery / tamper as lock events. */
    ESP_LOGI(TAG, "keypad %u: battery %u mV, flags 0x%02x", peer, battery_mv, flags);
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
    return ms > 3600000 ? pdMS_TO_TICKS(3600000) : pdMS_TO_TICKS((uint32_t)ms) + 1;
}

static uint64_t min_u64(uint64_t a, uint64_t b) { return a < b ? a : b; }

static void handle(const ev_t *ev)
{
    switch (ev->kind) {
    case EV_CALL:
        *ev->call.ret = ev->call.fn(&s_ctx, ev->call.arg);
        xTaskNotifyGive(ev->call.waiter);
        break;
    case EV_MOTOR_DONE:
        lc_on_motor_done(&s_ctx.lc, ev->motor_ok);
        break;
    case EV_DOOR:
        lc_on_door(&s_ctx.lc, ev->door);
        break;
    case EV_BOLT_POSITION:
        lc_on_bolt_position(&s_ctx.lc, ev->bolt);
        break;
    case EV_LINK_CONNECT:
        ll_on_connect(&s_ctx.ll, ev->link.conn);
        break;
    case EV_LINK_DISCONNECT:
        ll_on_disconnect(&s_ctx.ll, ev->link.conn);
        break;
    case EV_LINK_FRAME:
        ll_on_frame(&s_ctx.ll, ev->link.conn, ev->link.data, ev->link.len);
        break;
    }
}

static void lock_task(void *arg)
{
    (void)arg;
    static ev_t ev; /* large (frame buffer); keep off the stack */
    for (;;) {
        uint64_t now = now_ms();
        uint64_t deadline = min_u64(lc_next_deadline_ms(&s_ctx.lc), ll_next_deadline_ms(&s_ctx.ll));
        if (s_log_dirty) {
            deadline = min_u64(deadline, s_log_dirty_since_ms + LOG_FLUSH_DELAY_MS);
        }

        if (xQueueReceive(s_queue, &ev, ticks_until(deadline, now)) == pdTRUE) {
            handle(&ev);
        }

        lc_tick(&s_ctx.lc);
        ll_tick(&s_ctx.ll);

        bool pairing = ll_pairing_open(&s_ctx.ll);
        if (pairing != s_adv_pairing) {
            s_adv_pairing = pairing;
            ble_server_set_pairing(pairing);
            ESP_LOGI(TAG, "pairing window %s", pairing ? "open" : "closed");
        }
        if (s_log_dirty && now_ms() >= s_log_dirty_since_ms + LOG_FLUSH_DELAY_MS) {
            pe_store_save(&s_ctx.lc, LC_PERSIST_LOG);
            s_log_dirty = false;
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

    lc_init(&s_ctx.lc, &plat, &hooks, &cfg);
    ESP_ERROR_CHECK(pe_store_load(&s_ctx.lc));
    /* TODO(M1): read the real bolt position from the encoder. */
    lc_boot(&s_ctx.lc, LC_BOLT_LOCKED, LC_DOOR_UNKNOWN);

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
    };
    ll_init(&s_ctx.ll, &identity, &link_hooks);
    sl_wipe(&identity, sizeof(identity));
    pe_store_load_peers(&s_ctx.ll.peers, sizeof(s_ctx.ll.peers));
    ESP_LOGI(TAG, "%u paired peer(s); lock state %u bytes", ll_peer_count(&s_ctx.ll), (unsigned)sizeof(s_ctx));

    s_queue = xQueueCreate(16, sizeof(ev_t));
    configASSERT(s_queue);
    /* Noise handshakes (X25519) and PIN matching (HMAC per slot) need room. */
    xTaskCreate(lock_task, "lock", 10240, NULL, 5, NULL);
}

int lock_task_call(lock_task_fn_t fn, void *arg)
{
    int ret = -1;
    ev_t *ev = calloc(1, sizeof(*ev));
    configASSERT(ev);
    ev->kind = EV_CALL;
    ev->call.fn = fn;
    ev->call.arg = arg;
    ev->call.ret = &ret;
    ev->call.waiter = xTaskGetCurrentTaskHandle();
    xQueueSend(s_queue, ev, portMAX_DELAY);
    free(ev);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return ret;
}

static void post(ev_t *ev)
{
    if (xQueueSend(s_queue, ev, pdMS_TO_TICKS(100)) != pdTRUE) {
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
