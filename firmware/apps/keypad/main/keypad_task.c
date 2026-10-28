#include "keypad_task.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ble_central.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "feedback.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "keypad_core/kp.h"
#include "link_proto/lp.h"
#include "ln_store.h"
#include "sdkconfig.h"
#include "secure_link/sl_crypto.h"

static const char *const TAG = "keypad";

#define STORE_NS "keypad"

enum {
    MAX_WAIT_MS = 3600000, /* cap on one queue wait; the loop just re-arms */
    EVENT_QUEUE_LEN = 12,
    TASK_STACK = 8192,
    TASK_PRIO = 5,
    POST_TIMEOUT_MS = 100,
};

typedef enum { EV_KEY, EV_PAIR, EV_FORGET, EV_CONNECTED, EV_DISCONNECTED, EV_FRAME, EV_CALL } ev_kind_t;

typedef struct {
    ev_kind_t kind;
    union {
        char key;
        struct {
            uint16_t len;
            uint8_t data[LP_MAX_FRAME];
        } frame;
        struct {
            keypad_task_fn_t fn;
            void *arg;
            TaskHandle_t waiter;
        } call;
    };
} ev_t;

/* Owned by the keypad task; the queue handle is set once at start. */
static struct {
    kp_t kp;
    QueueHandle_t queue;
} s_kt; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): keypad task state

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

static void h_connect(void *ctx, const kp_pairing_t *pairing)
{
    (void)ctx;
    ble_central_connect(pairing);
}

static void h_disconnect(void *ctx)
{
    (void)ctx;
    ble_central_disconnect();
}

static bool h_send(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    return ble_central_send(frame, len);
}

static void h_feedback(void *ctx, kp_feedback_t feedback, uint16_t detail)
{
    (void)ctx;
    feedback_show(feedback, detail);
}

static void h_save_pairing(void *ctx, const kp_pairing_t *pairing)
{
    (void)ctx;
    esp_err_t err = pairing->paired ? ln_blob_save(STORE_NS, "pairing", pairing, sizeof(*pairing))
                              : ln_namespace_erase(STORE_NS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving pairing failed: %s", esp_err_to_name(err));
    }
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

static void keypad_task(void *arg)
{
    (void)arg;
    static ev_t ev;
    for (;;) {
        if (xQueueReceive(s_kt.queue, &ev, ticks_until(kp_next_deadline_ms(&s_kt.kp))) == pdTRUE) {
            switch (ev.kind) {
            case EV_KEY:
                kp_on_key(&s_kt.kp, ev.key);
                break;
            case EV_PAIR:
                kp_start_pairing(&s_kt.kp);
                break;
            case EV_FORGET:
                kp_forget_pairing(&s_kt.kp);
                ESP_LOGI(TAG, "pairing forgotten");
                break;
            case EV_CONNECTED:
                kp_on_connected(&s_kt.kp, ev.frame.data, ev.frame.len);
                break;
            case EV_DISCONNECTED:
                kp_on_disconnected(&s_kt.kp);
                break;
            case EV_FRAME:
                kp_on_frame(&s_kt.kp, ev.frame.data, ev.frame.len);
                break;
            case EV_CALL:
                ev.call.fn(&s_kt.kp, ev.call.arg);
                xTaskNotifyGive(ev.call.waiter);
                break;
            }
            sl_wipe(&ev, sizeof(ev));
        }
        kp_tick(&s_kt.kp);
        /* TODO(power): when kp_idle(), stop the radio and deep-sleep until a
         * key wakes us. */
    }
}

void keypad_task_start(void)
{
    /* Long-term identity. Must be created before the radio starts. */
    sl_keypair_t identity;
    ESP_ERROR_CHECK(ln_secret_get_or_create("link_id", identity.priv, sizeof(identity.priv)));
    sl_keypair_from_private(&identity);

    kp_pairing_t pairing = {0};
    if (ln_blob_load(STORE_NS, "pairing", &pairing, sizeof(pairing)) != ESP_OK) {
        memset(&pairing, 0, sizeof(pairing));
    }

    kp_hooks_t hooks = {
        .now_ms = h_now,
        .random = h_random,
        .connect = h_connect,
        .disconnect = h_disconnect,
        .send = h_send,
        .feedback = h_feedback,
        .save_pairing = h_save_pairing,
    };
    kp_init(&s_kt.kp, &identity, &pairing, &hooks);
    s_kt.kp.fw_major = CONFIG_KEYPAD_FW_MAJOR;
    s_kt.kp.fw_minor = CONFIG_KEYPAD_FW_MINOR;
    sl_wipe(&identity, sizeof(identity));
    ESP_LOGI(TAG, "keypad key %02x%02x%02x%02x…, %s", s_kt.kp.identity.pub[0], s_kt.kp.identity.pub[1],
             s_kt.kp.identity.pub[2], s_kt.kp.identity.pub[3], pairing.paired ? "paired" : "NOT paired");

    s_kt.queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(ev_t));
    configASSERT(s_kt.queue);
    xTaskCreate(keypad_task, "keypad", TASK_STACK, NULL, TASK_PRIO, NULL);
}

static void post(ev_t *ev)
{
    if (xQueueSend(s_kt.queue, ev, pdMS_TO_TICKS(POST_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "event queue full; dropped event %d", ev->kind);
    }
}

void keypad_task_post_key(char key)
{
    ev_t ev = {.kind = EV_KEY, .key = key};
    post(&ev);
}

void keypad_task_post_pair(void)
{
    ev_t ev = {.kind = EV_PAIR};
    post(&ev);
}

void keypad_task_post_forget(void)
{
    ev_t ev = {.kind = EV_FORGET};
    post(&ev);
}

void keypad_task_post_connected(const uint8_t *addr, size_t len)
{
    ev_t ev = {.kind = EV_CONNECTED};
    ev.frame.len = (uint16_t)(len > KP_ADDR_MAX ? KP_ADDR_MAX : len);
    memcpy(ev.frame.data, addr, ev.frame.len);
    post(&ev);
}

void keypad_task_post_disconnected(void)
{
    ev_t ev = {.kind = EV_DISCONNECTED};
    post(&ev);
}

void keypad_task_post_frame(const uint8_t *frame, size_t len)
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

void keypad_task_call(keypad_task_fn_t func, void *arg)
{
    ev_t *ev = calloc(1, sizeof(*ev));
    configASSERT(ev);
    ev->kind = EV_CALL;
    ev->call.fn = func;
    ev->call.arg = arg;
    ev->call.waiter = xTaskGetCurrentTaskHandle();
    xQueueSend(s_kt.queue, ev, portMAX_DELAY);
    free(ev);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}
