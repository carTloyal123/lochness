#include "hub_link.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hub_core/hub.h"
#include "link_proto/lp.h"
#include "ln_central.h"
#include "ln_store.h"
#include "secure_link/sl_crypto.h"

static const char *const TAG = "hub_link";

#define STORE_NS "hub"

enum {
    MAX_WAIT_MS = 3600000,
    EVENT_QUEUE_LEN = 12,
    TASK_STACK = 8192,
    TASK_PRIO = 5,
    POST_TIMEOUT_MS = 100,
};

typedef enum { EV_CONNECTED, EV_DISCONNECTED, EV_FRAME, EV_REQUEST, EV_PAIR, EV_FORGET, EV_TIME } ev_kind_t;

typedef struct {
    ev_kind_t kind;
    union {
        struct {
            uint16_t len;
            uint8_t data[LP_MAX_FRAME];
        } frame;
        struct {
            uint32_t id; /* matches waiter_t.id while the caller still waits */
            uint8_t op;
            uint8_t pin_len;
            uint8_t pin[LP_PIN_FIELD_LEN];
        } req;
    };
} ev_t;

/* The waiting Matter request, if any. Guarded by s_hl.lock. */
typedef struct {
    bool active;
    uint32_t id;
    uint8_t seq; /* 0 until the hub task has sent it */
    uint8_t result;
    uint8_t bolt;
} waiter_t;

static struct {
    hub_t hub;
    QueueHandle_t queue;
    SemaphoreHandle_t request_mutex; /* one Matter request at a time */
    SemaphoreHandle_t done;          /* given when the waiter has its answer */
    portMUX_TYPE lock;
    waiter_t waiter;
    uint32_t next_id;
    hub_link_cbs_t cbs;
    volatile bool ready;
} s_hl = {.lock = portMUX_INITIALIZER_UNLOCKED}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): module state

static uint64_t h_now(void *ctx)
{
    (void)ctx;
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static bool h_random(void *ctx, uint8_t *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len); /* radio is on, so the RNG is entropy-backed */
    return true;
}

static void h_connect(void *ctx, const uint8_t *addr, size_t len)
{
    (void)ctx;
    ln_central_connect(addr, len);
}

static void h_disconnect(void *ctx)
{
    (void)ctx;
    ln_central_disconnect();
}

static bool h_send(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    return ln_central_send(frame, len);
}

static void h_save_pairing(void *ctx, const hub_pairing_t *pairing)
{
    (void)ctx;
    esp_err_t err = pairing->paired ? ln_blob_save(STORE_NS, "pairing", pairing, sizeof(*pairing))
                                    : ln_namespace_erase(STORE_NS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving pairing failed: %s", esp_err_to_name(err));
    }
}

static bool h_wall_time(void *ctx, uint32_t *unix_s, int16_t *offset)
{
    (void)ctx;
    return s_hl.cbs.wall_time && s_hl.cbs.wall_time(unix_s, offset);
}

static void h_link_changed(void *ctx, bool is_up)
{
    (void)ctx;
    s_hl.ready = is_up;
    ESP_LOGI(TAG, "link to lock %s", is_up ? "UP" : "down");
    if (s_hl.cbs.link_changed) {
        s_hl.cbs.link_changed(is_up);
    }
}

static void h_lock_state(void *ctx, const lp_msg_t *state)
{
    (void)ctx;
    if (s_hl.cbs.lock_state) {
        s_hl.cbs.lock_state(state);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): mirrors hub_hooks_t.result
static void answer_waiter(uint8_t seq, uint8_t result, uint8_t bolt)
{
    bool give = false;
    portENTER_CRITICAL(&s_hl.lock);
    if (s_hl.waiter.active && s_hl.waiter.seq == seq) {
        s_hl.waiter.result = result;
        s_hl.waiter.bolt = bolt;
        s_hl.waiter.active = false;
        give = true;
    }
    portEXIT_CRITICAL(&s_hl.lock);
    if (give) {
        xSemaphoreGive(s_hl.done);
    }
}

static void h_result(void *ctx, uint8_t seq, uint8_t result, uint8_t bolt)
{
    (void)ctx;
    answer_waiter(seq, result, bolt);
}

static void h_paired(void *ctx, bool ok)
{
    (void)ctx;
    ESP_LOGI(TAG, "pairing %s", ok ? "PAIRED" : "failed");
}

static TickType_t ticks_until(uint64_t deadline_ms)
{
    if (deadline_ms == UINT64_MAX) {
        return portMAX_DELAY;
    }
    uint64_t now = h_now(NULL);
    if (deadline_ms <= now) {
        return 0;
    }
    uint64_t ms = deadline_ms - now;
    return ms > MAX_WAIT_MS ? pdMS_TO_TICKS(MAX_WAIT_MS) : pdMS_TO_TICKS((uint32_t)ms) + 1;
}

static void start_request(const ev_t *ev)
{
    /* A caller that already timed out must not have its unlock run late. */
    portENTER_CRITICAL(&s_hl.lock);
    bool current = s_hl.waiter.active && s_hl.waiter.id == ev->req.id && s_hl.waiter.seq == 0;
    portEXIT_CRITICAL(&s_hl.lock);
    if (!current) {
        ESP_LOGW(TAG, "dropped a request whose caller gave up");
        return;
    }
    uint8_t seq = hub_request(&s_hl.hub, (lp_op_t)ev->req.op, ev->req.pin_len ? ev->req.pin : NULL, ev->req.pin_len);
    if (seq == 0) {
        portENTER_CRITICAL(&s_hl.lock);
        bool waiting = s_hl.waiter.active && s_hl.waiter.id == ev->req.id && s_hl.waiter.seq == 0;
        if (waiting) {
            s_hl.waiter.result = LP_RESULT_ERROR;
            s_hl.waiter.bolt = LP_BOLT_UNKNOWN;
            s_hl.waiter.active = false;
        }
        portEXIT_CRITICAL(&s_hl.lock);
        if (waiting) {
            xSemaphoreGive(s_hl.done);
        }
        return;
    }
    portENTER_CRITICAL(&s_hl.lock);
    if (s_hl.waiter.active && s_hl.waiter.id == ev->req.id && s_hl.waiter.seq == 0) {
        s_hl.waiter.seq = seq;
    }
    portEXIT_CRITICAL(&s_hl.lock);
}

static void hub_task(void *arg)
{
    (void)arg;
    static ev_t ev;
    for (;;) {
        if (xQueueReceive(s_hl.queue, &ev, ticks_until(hub_next_deadline_ms(&s_hl.hub))) == pdTRUE) {
            switch (ev.kind) {
            case EV_CONNECTED:
                hub_on_connected(&s_hl.hub, ev.frame.data, ev.frame.len);
                break;
            case EV_DISCONNECTED:
                hub_on_disconnected(&s_hl.hub);
                break;
            case EV_FRAME:
                hub_on_frame(&s_hl.hub, ev.frame.data, ev.frame.len);
                break;
            case EV_REQUEST:
                start_request(&ev);
                break;
            case EV_PAIR:
                hub_start_pairing(&s_hl.hub);
                break;
            case EV_FORGET:
                hub_forget_pairing(&s_hl.hub);
                ESP_LOGI(TAG, "pairing forgotten");
                break;
            case EV_TIME:
                hub_send_time(&s_hl.hub);
                break;
            }
            sl_wipe(&ev, sizeof(ev));
        }
        hub_tick(&s_hl.hub);
    }
}

static void post(ev_t *ev)
{
    if (xQueueSend(s_hl.queue, ev, pdMS_TO_TICKS(POST_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "event queue full; dropped event %d", ev->kind);
    }
}

/* ln_central callbacks (NimBLE host task). */
static void on_connected(const uint8_t *addr, size_t len)
{
    ev_t ev = {.kind = EV_CONNECTED};
    ev.frame.len = (uint16_t)(len > HUB_ADDR_MAX ? HUB_ADDR_MAX : len);
    memcpy(ev.frame.data, addr, ev.frame.len);
    post(&ev);
}

static void on_disconnected(void)
{
    ev_t ev = {.kind = EV_DISCONNECTED};
    post(&ev);
}

static void on_frame(const uint8_t *frame, size_t len)
{
    if (len > LP_MAX_FRAME) {
        return;
    }
    static ev_t ev; /* only called from the NimBLE host task */
    ev.kind = EV_FRAME;
    ev.frame.len = (uint16_t)len;
    memcpy(ev.frame.data, frame, len);
    post(&ev);
}

void hub_link_start(const hub_link_cbs_t *cbs)
{
    s_hl.cbs = *cbs;
    s_hl.request_mutex = xSemaphoreCreateMutex();
    s_hl.done = xSemaphoreCreateBinary();
    configASSERT(s_hl.request_mutex && s_hl.done);

    /* Long-term identity. Created on first boot, before Matter starts the
     * radio (see app_main). */
    sl_keypair_t identity;
    ESP_ERROR_CHECK(ln_secret_get_or_create("link_id", identity.priv, sizeof(identity.priv)));
    sl_keypair_from_private(&identity);
    hub_pairing_t pairing = {0};
    if (ln_blob_load(STORE_NS, "pairing", &pairing, sizeof(pairing)) != ESP_OK) {
        memset(&pairing, 0, sizeof(pairing));
    }
    hub_hooks_t hooks = {
        .now_ms = h_now,
        .random = h_random,
        .connect = h_connect,
        .disconnect = h_disconnect,
        .send = h_send,
        .save_pairing = h_save_pairing,
        .wall_time = h_wall_time,
        .link_changed = h_link_changed,
        .lock_state = h_lock_state,
        .result = h_result,
        .paired = h_paired,
    };
    hub_init(&s_hl.hub, &identity, &pairing, &hooks);
    sl_wipe(&identity, sizeof(identity));
    ESP_LOGI(TAG, "hub key %02x%02x%02x%02x…, %s", s_hl.hub.identity.pub[0], s_hl.hub.identity.pub[1],
             s_hl.hub.identity.pub[2], s_hl.hub.identity.pub[3], pairing.paired ? "paired" : "NOT paired");

    static const ln_central_cbs_t CENTRAL = {
        .connected = on_connected, .disconnected = on_disconnected, .frame = on_frame};
    ESP_ERROR_CHECK(ln_central_start(&CENTRAL, false)); /* Matter owns the NimBLE host */

    s_hl.queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(ev_t));
    configASSERT(s_hl.queue);
    hub_start(&s_hl.hub);
    xTaskCreate(hub_task, "hub_link", TASK_STACK, NULL, TASK_PRIO, NULL);
}

uint8_t hub_link_request(lp_op_t op, const uint8_t *pin, size_t pin_len, uint32_t timeout_ms, uint8_t *bolt)
{
    if (pin_len > LP_PIN_FIELD_LEN || !s_hl.queue) {
        return LP_RESULT_ERROR;
    }
    xSemaphoreTake(s_hl.request_mutex, portMAX_DELAY);
    xSemaphoreTake(s_hl.done, 0); /* drop an answer that came after a timeout */
    portENTER_CRITICAL(&s_hl.lock);
    uint32_t id = ++s_hl.next_id;
    s_hl.waiter = (waiter_t){.active = true, .id = id, .result = LP_RESULT_ERROR, .bolt = LP_BOLT_UNKNOWN};
    portEXIT_CRITICAL(&s_hl.lock);

    ev_t ev = {.kind = EV_REQUEST, .req = {.id = id, .op = (uint8_t)op, .pin_len = (uint8_t)pin_len}};
    if (pin_len) {
        memcpy(ev.req.pin, pin, pin_len);
    }
    post(&ev);
    sl_wipe(&ev, sizeof(ev));

    bool answered = xSemaphoreTake(s_hl.done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    portENTER_CRITICAL(&s_hl.lock);
    waiter_t waiter = s_hl.waiter;
    s_hl.waiter.active = false; /* a late answer is ignored */
    portEXIT_CRITICAL(&s_hl.lock);
    xSemaphoreGive(s_hl.request_mutex);
    if (bolt) {
        *bolt = answered ? waiter.bolt : LP_BOLT_UNKNOWN;
    }
    return answered ? waiter.result : LP_RESULT_ERROR;
}

void hub_link_pair(void)
{
    ev_t ev = {.kind = EV_PAIR};
    post(&ev);
}

void hub_link_forget(void)
{
    ev_t ev = {.kind = EV_FORGET};
    post(&ev);
}

void hub_link_time_changed(void)
{
    if (s_hl.queue) {
        ev_t ev = {.kind = EV_TIME};
        post(&ev);
    }
}

bool hub_link_ready(void) { return s_hl.ready; }

void hub_link_print_status(void)
{
    /* Racy snapshot for the console; fine for display. */
    const hub_t *hub = &s_hl.hub;
    static const char *const LINKS[] = {"down", "waiting to retry", "connecting", "handshake", "ready"};
    printf("hub key  %02x%02x%02x%02x…\n", hub->identity.pub[0], hub->identity.pub[1], hub->identity.pub[2],
           hub->identity.pub[3]);
    if (hub->pairing.paired) {
        /* Transport address: type byte, then the BLE address LSB first. */
        char addr[sizeof("00:00:00:00:00:00")] = "?";
        if (hub->pairing.lock_addr_len == LN_CENTRAL_ADDR_LEN) {
            char *out = addr;
            for (size_t i = LN_CENTRAL_ADDR_LEN - 1; i >= 1; i--) {
                out += snprintf(out, (size_t)(addr + sizeof(addr) - out), i > 1 ? "%02x:" : "%02x",
                                hub->pairing.lock_addr[i]);
            }
        }
        printf("lock     %02x%02x%02x%02x… at %s\n", hub->pairing.lock_pub[0], hub->pairing.lock_pub[1],
               hub->pairing.lock_pub[2], hub->pairing.lock_pub[3], addr);
    } else {
        printf("lock     not paired\n");
    }
    printf("link     %s%s\n", hub->link <= HUB_LINK_READY ? LINKS[hub->link] : "?",
           hub->pairing_mode ? " (pairing)" : "");
}
