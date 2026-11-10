#include "ble_server.h"

#include <stdint.h>

#include "sdkconfig.h"

#if CONFIG_BT_ENABLED

#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "link_proto/lp.h"
#include "link_proto/lp_ble.h"
#include "lock_task.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"

static const char *const TAG = "ble";

static const ble_uuid128_t SVC_UUID = {.u = {.type = BLE_UUID_TYPE_128}, .value = LP_BLE_SERVICE_UUID_LE};
static const ble_uuid128_t RX_UUID = {.u = {.type = BLE_UUID_TYPE_128}, .value = LP_BLE_RX_UUID_LE};
static const ble_uuid128_t TX_UUID = {.u = {.type = BLE_UUID_TYPE_128}, .value = LP_BLE_TX_UUID_LE};

/* Service data = 16-byte service UUID + one flags byte. */
enum { SVC_DATA_LEN = 17, SVC_DATA_FLAGS = 16 };

/* Touched from the NimBLE host task; `pairing` is also set by the lock task. */
static struct {
    uint16_t tx_handle;
    uint8_t own_addr_type;
    volatile bool pairing;
    int connections;
} s_ble; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): module state

static void advertise(void);

static int rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t frame[LP_MAX_FRAME];
    // NOLINTNEXTLINE(bugprone-casting-through-void): inside NimBLE's OS_MBUF_PKTLEN macro
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > sizeof(frame)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, frame, sizeof(frame), &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    lock_task_post_link_frame(conn, frame, len);
    return 0;
}

static int tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)ctxt;
    (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static const struct ble_gatt_svc_def SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {.uuid = &RX_UUID.u, .access_cb = rx_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
                {.uuid = &TX_UUID.u, .access_cb = tx_access, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_ble.tx_handle},
                {0},
            },
    },
    {0},
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_ble.connections++;
            lock_task_post_link_connect(event->connect.conn_handle);
        }
        advertise(); /* keep accepting other peers up to the connection limit */
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_ble.connections--;
        lock_task_post_link_disconnect(event->disconnect.conn.conn_handle);
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGD(TAG, "conn %u mtu %u", event->mtu.conn_handle, event->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    if (s_ble.connections >= CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
        return;
    }
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
    /* Service data = UUID (16 bytes) + flags. No name: the lock does not
     * announce who it belongs to. */
    static uint8_t svc_data[SVC_DATA_LEN] = LP_BLE_SERVICE_UUID_LE;
    svc_data[SVC_DATA_FLAGS] = s_ble.pairing ? LP_BLE_ADV_FLAG_PAIRING : 0;

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.svc_data_uuid128 = svc_data;
    fields.svc_data_uuid128_len = sizeof(svc_data);
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields: %d", rc);
        return;
    }
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        /* The keypad starts connecting on the first key press, so most of
         * this wait hides behind typing; one-touch lock sees all of it. */
        .itvl_min = BLE_GAP_ADV_ITVL_MS(CONFIG_LOCHNESS_ADV_INTERVAL_MS),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(CONFIG_LOCHNESS_ADV_INTERVAL_MS + CONFIG_LOCHNESS_ADV_INTERVAL_MS / 10),
    };
    if (s_ble.pairing) {
        params.itvl_min = BLE_GAP_ADV_ITVL_MS(100);
        params.itvl_max = BLE_GAP_ADV_ITVL_MS(150);
    }
    rc = ble_gap_adv_start(s_ble.own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start: %d", rc);
    }
}

static void on_sync(void)
{
    /* TODO(privacy): rotate addresses and replace the static service data
     * with a tag only paired peers can recognize. */
    ESP_ERROR_CHECK(ble_hs_util_ensure_addr(0));
    ESP_ERROR_CHECK(ble_hs_id_infer_auto(0, &s_ble.own_addr_type));
    uint8_t addr[BLE_DEV_ADDR_LEN];
    ble_hs_id_copy_addr(s_ble.own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "advertising as %02x:%02x:%02x:%02x:%02x:%02x", addr[5], addr[4], addr[3], addr[2],
             addr[1], addr[0]);
    advertise();
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

esp_err_t ble_server_start(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_att_set_preferred_mtu(CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU);

    int rc = ble_gatts_count_cfg(SERVICES);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(SERVICES);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT registration failed: %d", rc);
        return ESP_FAIL;
    }
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

bool ble_server_send(uint16_t conn, const uint8_t *frame, size_t len)
{
    struct os_mbuf *mbuf = ble_hs_mbuf_from_flat(frame, (uint16_t)len);
    if (!mbuf) {
        return false;
    }
    return ble_gatts_notify_custom(conn, s_ble.tx_handle, mbuf) == 0;
}

void ble_server_disconnect(uint16_t conn) { ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM); }

void ble_server_set_pairing(bool open)
{
    if (s_ble.pairing != open) {
        s_ble.pairing = open;
        if (ble_hs_synced()) {
            advertise();
        }
    }
}

#else /* !CONFIG_BT_ENABLED: QEMU smoke-test builds have no radio */

#include "esp_log.h"

esp_err_t ble_server_start(void)
{
    ESP_LOGW("ble", "Bluetooth disabled in this build; link unavailable");
    return ESP_OK;
}
bool ble_server_send(uint16_t conn, const uint8_t *frame, size_t len)
{
    (void)conn;
    (void)frame;
    (void)len;
    return false;
}
void ble_server_disconnect(uint16_t conn) { (void)conn; }
void ble_server_set_pairing(bool open) { (void)open; }

#endif
