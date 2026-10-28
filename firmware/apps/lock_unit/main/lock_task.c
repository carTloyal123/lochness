#include "lock_task.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
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
    };
} ev_t;

static lc_t s_lc;
static QueueHandle_t s_queue;
static bool s_log_dirty;
static uint64_t s_log_dirty_since_ms;

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
    /* TODO(M2/M3): forward to the hub and connected app over secure_link. */
    ESP_LOGI(TAG, "event: %s (source %d, user %u)", event_name(ev->type), ev->source,
             ev->user_index);
}

static void hook_persist(void *ctx, lc_persist_t what)
{
    (void)ctx;
    if (what == LC_PERSIST_LOG) {
        if (!s_log_dirty) {
            s_log_dirty = true;
            s_log_dirty_since_ms = s_lc.plat.now_ms(NULL);
        }
        return;
    }
    pe_store_save(&s_lc, what);
}

void lock_task_save_config(const lc_t *lc) { pe_store_save_config(&lc->cfg); }

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

static void lock_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint64_t now = s_lc.plat.now_ms(NULL);
        uint64_t deadline = lc_next_deadline_ms(&s_lc);
        if (s_log_dirty) {
            uint64_t flush_at = s_log_dirty_since_ms + LOG_FLUSH_DELAY_MS;
            deadline = flush_at < deadline ? flush_at : deadline;
        }

        ev_t ev;
        if (xQueueReceive(s_queue, &ev, ticks_until(deadline, now)) == pdTRUE) {
            switch (ev.kind) {
            case EV_CALL:
                *ev.call.ret = ev.call.fn(&s_lc, ev.call.arg);
                xTaskNotifyGive(ev.call.waiter);
                break;
            case EV_MOTOR_DONE:
                lc_on_motor_done(&s_lc, ev.motor_ok);
                break;
            case EV_DOOR:
                lc_on_door(&s_lc, ev.door);
                break;
            case EV_BOLT_POSITION:
                lc_on_bolt_position(&s_lc, ev.bolt);
                break;
            }
        }

        lc_tick(&s_lc);
        if (s_log_dirty && s_lc.plat.now_ms(NULL) >= s_log_dirty_since_ms + LOG_FLUSH_DELAY_MS) {
            pe_store_save(&s_lc, LC_PERSIST_LOG);
            s_log_dirty = false;
        }
    }
}

void lock_task_start(void)
{
    lc_platform_t plat = pe_platform();
    lc_hooks_t hooks = {
        .ctx = NULL,
        .motor_start = motor_sim_start,
        .motor_stop = motor_sim_stop,
        .notify = hook_notify,
        .persist = hook_persist,
    };
    lc_config_t cfg = lc_config_default();
    pe_store_load_config(&cfg);

    lc_init(&s_lc, &plat, &hooks, &cfg);
    ESP_ERROR_CHECK(pe_store_load(&s_lc));
    /* TODO(M1): read the real bolt position from the encoder. */
    lc_boot(&s_lc, LC_BOLT_LOCKED, LC_DOOR_UNKNOWN);
    ESP_LOGI(TAG, "lock_core state is %u bytes", (unsigned)sizeof(s_lc));

    s_queue = xQueueCreate(16, sizeof(ev_t));
    configASSERT(s_queue);
    /* PIN matching runs HMAC over every credential slot; give it room. */
    xTaskCreate(lock_task, "lock", 8192, NULL, 5, NULL);
}

int lock_task_call(lock_task_fn_t fn, void *arg)
{
    int ret = -1;
    ev_t ev = {.kind = EV_CALL,
               .call = {.fn = fn, .arg = arg, .ret = &ret, .waiter = xTaskGetCurrentTaskHandle()}};
    xQueueSend(s_queue, &ev, portMAX_DELAY);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return ret;
}

static void post(const ev_t *ev)
{
    if (xQueueSend(s_queue, ev, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "event queue full; dropped event %d", ev->kind);
    }
}

void lock_task_post_motor_done(bool success) { post(&(ev_t){.kind = EV_MOTOR_DONE, .motor_ok = success}); }
void lock_task_post_door(lc_door_t door) { post(&(ev_t){.kind = EV_DOOR, .door = door}); }
void lock_task_post_bolt_position(lc_bolt_t bolt) { post(&(ev_t){.kind = EV_BOLT_POSITION, .bolt = bolt}); }
