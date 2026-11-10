#include "ln_central.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "link_proto/lp.h"
#include "link_proto/lp_ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "sdkconfig.h"

static const char *const TAG = "central";

/* Links last a few seconds, so ask for the fastest interval: each GATT
 * round trip during setup costs one or two intervals. Units of 1.25 ms. */
static const struct ble_gap_conn_params CONN_PARAMS = {
    .scan_itvl = 0x0010, /* 10 ms window every 10 ms: continuous scan */
    .scan_window = 0x0010,
    .itvl_min = 6,  /* 7.5 ms */
    .itvl_max = 12, /* 15 ms */
    .latency = 0,
    .supervision_timeout = 100, /* 1 s, units of 10 ms */
    .min_ce_len = 0,
    .max_ce_len = 0,
};

enum {
    CONNECT_TIMEOUT_MS = 5000,
    SCAN_TIMEOUT_MS = 30000, /* matches KP_PAIR_TIMEOUT_MS */
    LOCK_ADDR_LEN = LN_CENTRAL_ADDR_LEN,
    /* Advertised service data: 128-bit service UUID, then one flags byte. */
    ADV_UUID_LEN = 16,
    ADV_FLAGS_OFFSET = ADV_UUID_LEN,
    ADV_SVC_DATA_LEN = ADV_UUID_LEN + 1,
};

static const ble_uuid128_t SVC_UUID = {.u = {.type = BLE_UUID_TYPE_128}, .value = LP_BLE_SERVICE_UUID_LE};
static const ble_uuid128_t RX_UUID = {.u = {.type = BLE_UUID_TYPE_128}, .value = LP_BLE_RX_UUID_LE};
static const ble_uuid128_t TX_UUID = {.u = {.type = BLE_UUID_TYPE_128}, .value = LP_BLE_TX_UUID_LE};
static const uint8_t SVC_UUID_LE[ADV_UUID_LEN] = LP_BLE_SERVICE_UUID_LE;

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

/* Module state, guarded by mutex. */
static struct {
    SemaphoreHandle_t mutex;
    bc_state_t state;
    uint16_t conn;
    ble_addr_t peer;
    uint16_t svc_end;
    uint16_t rx_handle;
    uint16_t tx_handle;
    uint16_t cccd_handle;
    request_t pending;
    uint8_t own_addr_type;
    bool synced;
    ln_central_cbs_t cbs;
} s_bc; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): guarded by s_bc.mutex

static int gap_event(struct ble_gap_event *event, void *arg);

/* Recursive: NimBLE may invoke gap_event from inside a call we make while locked. */
static void lock(void) { xSemaphoreTakeRecursive(s_bc.mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(s_bc.mutex); }

/* Called with s_bc.mutex held. */
static void start(const request_t *req)
{
    int rc = 0;
    if (req->scan) {
        struct ble_gap_disc_params params = {.passive = 1, .filter_duplicates = 1};
        rc = ble_gap_disc(s_bc.own_addr_type, SCAN_TIMEOUT_MS, &params, gap_event, NULL);
        s_bc.state = rc == 0 ? BC_SCANNING : BC_IDLE;
    } else {
        s_bc.peer = req->addr;
        rc = ble_gap_connect(s_bc.own_addr_type, &req->addr, CONNECT_TIMEOUT_MS, &CONN_PARAMS, gap_event, NULL);
        s_bc.state = rc == 0 ? BC_CONNECTING : BC_IDLE;
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "%s failed: %d", req->scan ? "scan" : "connect", rc);
        s_bc.cbs.disconnected();
    }
}

/* Called with s_bc.mutex held, once the radio is idle. */
static void become_idle(bool report)
{
    s_bc.state = BC_IDLE;
    s_bc.conn = BLE_HS_CONN_HANDLE_NONE;
    if (report) {
        s_bc.cbs.disconnected();
    }
    if (s_bc.pending.valid) {
        request_t req = s_bc.pending;
        s_bc.pending.valid = false;
        start(&req);
    }
}

void ln_central_connect(const uint8_t *addr, size_t addr_len)
{
    request_t req = {.valid = true, .scan = addr == NULL};
    if (addr) {
        if (addr_len != LOCK_ADDR_LEN) {
            s_bc.cbs.disconnected();
            return;
        }
        req.addr.type = addr[0];
        memcpy(req.addr.val, addr + 1, BLE_DEV_ADDR_LEN);
    }
    lock();
    if (!s_bc.synced) {
        unlock();
        s_bc.cbs.disconnected();
        return;
    }
    if (s_bc.state == BC_IDLE) {
        start(&req);
    } else {
        s_bc.pending = req; /* run once the previous link has torn down */
    }
    unlock();
}

void ln_central_disconnect(void)
{
    lock();
    s_bc.pending.valid = false;
    switch (s_bc.state) {
    case BC_SCANNING:
        ble_gap_disc_cancel();
        s_bc.state = BC_IDLE;
        break;
    case BC_CONNECTING:
        if (ble_gap_conn_cancel() == 0) {
            s_bc.state = BC_CLOSING;
        } else {
            s_bc.state = BC_IDLE;
        }
        break;
    case BC_DISCOVERING:
    case BC_READY:
        s_bc.state = BC_CLOSING;
        ble_gap_terminate(s_bc.conn, BLE_ERR_REM_USER_CONN_TERM);
        break;
    default:
        break;
    }
    unlock();
}

bool ln_central_send(const uint8_t *frame, size_t len)
{
    lock();
    bool ok = s_bc.state == BC_READY &&
              ble_gattc_write_no_rsp_flat(s_bc.conn, s_bc.rx_handle, frame, (uint16_t)len) == 0;
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
    if (s_bc.state == BC_DISCOVERING && conn == s_bc.conn) {
        s_bc.state = BC_READY;
        uint8_t addr[LOCK_ADDR_LEN] = {s_bc.peer.type};
        memcpy(addr + 1, s_bc.peer.val, BLE_DEV_ADDR_LEN);
        s_bc.cbs.connected(addr, sizeof(addr));
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
        if (ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0 && !s_bc.cccd_handle) {
            s_bc.cccd_handle = dsc->handle;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE || !s_bc.cccd_handle) {
        setup_failed(conn, "descriptor discovery", err->status);
        return 0;
    }
    static const uint8_t NOTIFY_ON[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(conn, s_bc.cccd_handle, NOTIFY_ON, sizeof(NOTIFY_ON), on_subscribed, NULL);
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
            s_bc.rx_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &TX_UUID.u) == 0) {
            s_bc.tx_handle = chr->val_handle;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE || !s_bc.rx_handle || !s_bc.tx_handle) {
        setup_failed(conn, "characteristic discovery", err->status);
        return 0;
    }
    int rc = ble_gattc_disc_all_dscs(conn, s_bc.tx_handle, s_bc.svc_end, on_dsc, NULL);
    if (rc != 0) {
        setup_failed(conn, "descriptor discovery", rc);
    }
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (err->status == 0) {
        s_bc.svc_end = svc->end_handle;
        int rc = ble_gattc_disc_all_chrs(conn, svc->start_handle, svc->end_handle, on_chr, NULL);
        if (rc != 0) {
            setup_failed(conn, "characteristic discovery", rc);
        }
        return BLE_HS_EDONE; /* only one instance expected */
    }
    if (err->status == BLE_HS_EDONE && !s_bc.svc_end) {
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
    s_bc.svc_end = s_bc.rx_handle = s_bc.tx_handle = s_bc.cccd_handle = 0;
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
    return fields.svc_data_uuid128_len == ADV_SVC_DATA_LEN &&
           memcmp(fields.svc_data_uuid128, SVC_UUID_LE, ADV_UUID_LEN) == 0 &&
           (fields.svc_data_uuid128[ADV_FLAGS_OFFSET] & (unsigned)LP_BLE_ADV_FLAG_PAIRING);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    lock();
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (s_bc.state == BC_SCANNING && is_pairing_lock(&event->disc)) {
            ble_gap_disc_cancel();
            s_bc.peer = event->disc.addr;
            int rc = ble_gap_connect(s_bc.own_addr_type, &s_bc.peer, CONNECT_TIMEOUT_MS, &CONN_PARAMS, gap_event, NULL);
            if (rc == 0) {
                s_bc.state = BC_CONNECTING;
            } else {
                become_idle(true);
            }
        }
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_bc.state == BC_SCANNING) {
            become_idle(true); /* nothing found in time */
        }
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            become_idle(s_bc.state != BC_CLOSING);
        } else if (s_bc.state == BC_CLOSING) {
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        } else {
            s_bc.conn = event->connect.conn_handle;
            s_bc.state = BC_DISCOVERING;
            int rc = ble_gattc_exchange_mtu(s_bc.conn, on_mtu, NULL);
            if (rc != 0) {
                setup_failed(s_bc.conn, "MTU exchange", rc);
            }
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        become_idle(s_bc.state != BC_CLOSING);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (s_bc.state == BC_READY && event->notify_rx.attr_handle == s_bc.tx_handle) {
            uint8_t frame[LP_MAX_FRAME];
            // NOLINTNEXTLINE(bugprone-casting-through-void): OS_MBUF_PKTLEN casts through void * in os_mbuf.h
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (len <= sizeof(frame) && ble_hs_mbuf_to_flat(event->notify_rx.om, frame, sizeof(frame), &len) == 0) {
                s_bc.cbs.frame(frame, len);
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
    ESP_ERROR_CHECK(ble_hs_id_infer_auto(0, &s_bc.own_addr_type));
    lock();
    s_bc.synced = true;
    unlock();
    ESP_LOGI(TAG, "BLE ready");
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset: %d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ln_central_start(const ln_central_cbs_t *cbs)
{
    s_bc.mutex = xSemaphoreCreateRecursiveMutex();
    configASSERT(s_bc.mutex);
    s_bc.conn = BLE_HS_CONN_HANDLE_NONE;
    s_bc.cbs = *cbs;
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
