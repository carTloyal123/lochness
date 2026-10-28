#include "ble_central.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "keypad_task.h"
#include "link_proto/lp.h"
#include "link_proto/lp_ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "sdkconfig.h"

static const char *TAG = "ble";

#define CONNECT_TIMEOUT_MS 5000
#define SCAN_TIMEOUT_MS KP_PAIR_TIMEOUT_MS

static const ble_uuid128_t SVC_UUID = {.u = {.type = BLE_UUID_TYPE_128}, .value = LP_BLE_SERVICE_UUID_LE};
static const ble_uuid128_t RX_UUID = {.u = {.type = BLE_UUID_TYPE_128}, .value = LP_BLE_RX_UUID_LE};
static const ble_uuid128_t TX_UUID = {.u = {.type = BLE_UUID_TYPE_128}, .value = LP_BLE_TX_UUID_LE};
static const uint8_t SVC_UUID_LE[16] = LP_BLE_SERVICE_UUID_LE;

typedef enum {
    BC_IDLE,
    BC_SCANNING,
    BC_CONNECTING,
    BC_DISCOVERING,
    BC_READY,
    BC_CLOSING, /* local close in progress; suppress the resulting event */
} bc_state_t;

typedef struct {
    bool valid;
    bool scan; /* pairing scan instead of direct connect */
    ble_addr_t addr;
} request_t;

static SemaphoreHandle_t s_lock;
static bc_state_t s_state;
static uint16_t s_conn;
static ble_addr_t s_peer;
static uint16_t s_svc_end, s_rx_handle, s_tx_handle, s_cccd_handle;
static request_t s_pending;
static uint8_t s_own_addr_type;
static bool s_synced;

static int gap_event(struct ble_gap_event *event, void *arg);

/* Recursive: NimBLE may invoke gap_event from inside a call we make while locked. */
static void lock(void) { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(s_lock); }

/* Called with s_lock held. */
static void start(const request_t *req)
{
    int rc;
    if (req->scan) {
        struct ble_gap_disc_params params = {.passive = 1, .filter_duplicates = 1};
        rc = ble_gap_disc(s_own_addr_type, SCAN_TIMEOUT_MS, &params, gap_event, NULL);
        s_state = rc == 0 ? BC_SCANNING : BC_IDLE;
    } else {
        s_peer = req->addr;
        rc = ble_gap_connect(s_own_addr_type, &req->addr, CONNECT_TIMEOUT_MS, NULL, gap_event, NULL);
        s_state = rc == 0 ? BC_CONNECTING : BC_IDLE;
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "%s failed: %d", req->scan ? "scan" : "connect", rc);
        keypad_task_post_disconnected();
    }
}

/* Called with s_lock held, once the radio is idle. */
static void become_idle(bool report)
{
    s_state = BC_IDLE;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    if (report) {
        keypad_task_post_disconnected();
    }
    if (s_pending.valid) {
        request_t req = s_pending;
        s_pending.valid = false;
        start(&req);
    }
}

void ble_central_connect(const kp_pairing_t *pairing)
{
    request_t req = {.valid = true, .scan = pairing == NULL};
    if (pairing) {
        if (pairing->lock_addr_len != 7) {
            keypad_task_post_disconnected();
            return;
        }
        req.addr.type = pairing->lock_addr[0];
        memcpy(req.addr.val, pairing->lock_addr + 1, 6);
    }
    lock();
    if (!s_synced) {
        unlock();
        keypad_task_post_disconnected();
        return;
    }
    if (s_state == BC_IDLE) {
        start(&req);
    } else {
        s_pending = req; /* run once the previous link has torn down */
    }
    unlock();
}

void ble_central_disconnect(void)
{
    lock();
    s_pending.valid = false;
    switch (s_state) {
    case BC_SCANNING:
        ble_gap_disc_cancel();
        s_state = BC_IDLE;
        break;
    case BC_CONNECTING:
        if (ble_gap_conn_cancel() == 0) {
            s_state = BC_CLOSING;
        } else {
            s_state = BC_IDLE;
        }
        break;
    case BC_DISCOVERING:
    case BC_READY:
        s_state = BC_CLOSING;
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        break;
    default:
        break;
    }
    unlock();
}

bool ble_central_send(const uint8_t *frame, size_t len)
{
    lock();
    bool ok = s_state == BC_READY &&
              ble_gattc_write_no_rsp_flat(s_conn, s_rx_handle, frame, (uint16_t)len) == 0;
    unlock();
    return ok;
}

/* ---- discovery chain: MTU -> service -> characteristics -> CCCD -> subscribe */

static void setup_failed(uint16_t conn, const char *what, int status)
{
    ESP_LOGW(TAG, "%s failed: %d", what, status);
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM); /* DISCONNECT event reports it */
}

static int on_subscribed(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    (void)arg;
    if (err->status != 0) {
        setup_failed(conn, "subscribe", err->status);
        return 0;
    }
    lock();
    if (s_state == BC_DISCOVERING && conn == s_conn) {
        s_state = BC_READY;
        uint8_t addr[7] = {s_peer.type};
        memcpy(addr + 1, s_peer.val, 6);
        keypad_task_post_connected(addr, sizeof(addr));
    }
    unlock();
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val, const struct ble_gatt_dsc *dsc,
                  void *arg)
{
    (void)chr_val;
    (void)arg;
    if (err->status == 0) {
        if (ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0 && !s_cccd_handle) {
            s_cccd_handle = dsc->handle;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE || !s_cccd_handle) {
        setup_failed(conn, "descriptor discovery", err->status);
        return 0;
    }
    static const uint8_t notify_on[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(conn, s_cccd_handle, notify_on, sizeof(notify_on), on_subscribed, NULL);
    if (rc != 0) {
        setup_failed(conn, "subscribe", rc);
    }
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (err->status == 0) {
        if (ble_uuid_cmp(&chr->uuid.u, &RX_UUID.u) == 0) {
            s_rx_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &TX_UUID.u) == 0) {
            s_tx_handle = chr->val_handle;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE || !s_rx_handle || !s_tx_handle) {
        setup_failed(conn, "characteristic discovery", err->status);
        return 0;
    }
    int rc = ble_gattc_disc_all_dscs(conn, s_tx_handle, s_svc_end, on_dsc, NULL);
    if (rc != 0) {
        setup_failed(conn, "descriptor discovery", rc);
    }
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (err->status == 0) {
        s_svc_end = svc->end_handle;
        int rc = ble_gattc_disc_all_chrs(conn, svc->start_handle, svc->end_handle, on_chr, NULL);
        if (rc != 0) {
            setup_failed(conn, "characteristic discovery", rc);
        }
        return BLE_HS_EDONE; /* only one instance expected */
    }
    if (err->status == BLE_HS_EDONE && !s_svc_end) {
        setup_failed(conn, "service discovery (not a lochness lock?)", err->status);
    } else if (err->status != BLE_HS_EDONE) {
        setup_failed(conn, "service discovery", err->status);
    }
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    (void)arg;
    if (err->status != 0 || mtu < LP_BLE_MIN_MTU) {
        setup_failed(conn, "MTU exchange", err->status ? err->status : mtu);
        return 0;
    }
    s_svc_end = s_rx_handle = s_tx_handle = s_cccd_handle = 0;
    int rc = ble_gattc_disc_svc_by_uuid(conn, &SVC_UUID.u, on_svc, NULL);
    if (rc != 0) {
        setup_failed(conn, "service discovery", rc);
    }
    return 0;
}

/* ---- GAP ------------------------------------------------------------------- */

static bool is_pairing_lock(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        return false;
    }
    return fields.svc_data_uuid128_len == 17 && memcmp(fields.svc_data_uuid128, SVC_UUID_LE, 16) == 0 &&
           (fields.svc_data_uuid128[16] & LP_BLE_ADV_FLAG_PAIRING);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    lock();
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (s_state == BC_SCANNING && is_pairing_lock(&event->disc)) {
            ble_gap_disc_cancel();
            s_peer = event->disc.addr;
            int rc = ble_gap_connect(s_own_addr_type, &s_peer, CONNECT_TIMEOUT_MS, NULL, gap_event, NULL);
            if (rc == 0) {
                s_state = BC_CONNECTING;
            } else {
                become_idle(true);
            }
        }
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_state == BC_SCANNING) {
            become_idle(true); /* nothing found in time */
        }
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            become_idle(s_state != BC_CLOSING);
        } else if (s_state == BC_CLOSING) {
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        } else {
            s_conn = event->connect.conn_handle;
            s_state = BC_DISCOVERING;
            int rc = ble_gattc_exchange_mtu(s_conn, on_mtu, NULL);
            if (rc != 0) {
                setup_failed(s_conn, "MTU exchange", rc);
            }
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        become_idle(s_state != BC_CLOSING);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (s_state == BC_READY && event->notify_rx.attr_handle == s_tx_handle) {
            uint8_t frame[LP_MAX_FRAME];
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (len <= sizeof(frame) && ble_hs_mbuf_to_flat(event->notify_rx.om, frame, sizeof(frame), &len) == 0) {
                keypad_task_post_frame(frame, len);
            }
        }
        break;
    }
    default:
        break;
    }
    unlock();
    return 0;
}

static void on_sync(void)
{
    ESP_ERROR_CHECK(ble_hs_util_ensure_addr(0));
    ESP_ERROR_CHECK(ble_hs_id_infer_auto(0, &s_own_addr_type));
    lock();
    s_synced = true;
    unlock();
    ESP_LOGI(TAG, "BLE ready");
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset: %d", reason); }

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_central_start(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    configASSERT(s_lock);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_att_set_preferred_mtu(CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU);
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
