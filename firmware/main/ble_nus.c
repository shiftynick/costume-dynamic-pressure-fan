#include "ble_nus.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_nus";

// Nordic UART Service UUIDs (128-bit, little-endian byte order for NimBLE):
//   Service:  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//   RX char:  6E400002-...    (write from phone)
//   TX char:  6E400003-...    (notify to phone)
static const ble_uuid128_t NUS_SVC_UUID = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_RX_UUID = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t NUS_TX_UUID = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

static uint16_t        s_conn_handle    = BLE_HS_CONN_HANDLE_NONE;
static uint16_t        s_tx_attr_handle = 0;
static ble_nus_rx_cb_t s_rx_cb          = NULL;
static char            s_device_name[24] = "graboid-01";
static uint8_t         s_own_addr_type;

static int gap_event_handler(struct ble_gap_event *event, void *arg);
static void advertise(void);

static int nus_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return 0;
    }
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    char buf[160];
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    int rc = os_mbuf_copydata(ctxt->om, 0, len, buf);
    if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
    buf[len] = '\0';
    if (s_rx_cb) s_rx_cb(buf, len);
    return 0;
}

static int nus_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    // Notify-only; clients subscribe via the auto-managed CCCD, never
    // read/write the value directly. Stub here for NimBLE's table.
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid      = &NUS_RX_UUID.u,
                .access_cb = nus_rx_access,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid       = &NUS_TX_UUID.u,
                .access_cb  = nus_tx_access,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_attr_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

static void advertise(void)
{
    struct ble_gap_adv_params adv_params = {0};
    struct ble_hs_adv_fields  fields     = {0};
    struct ble_hs_adv_fields  rsp        = {0};

    fields.flags                = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.uuids128              = (ble_uuid128_t[]){ NUS_SVC_UUID };
    fields.num_uuids128          = 1;
    fields.uuids128_is_complete  = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGW(TAG, "adv_set_fields rc=%d", rc); return; }

    rsp.name             = (uint8_t *)s_device_name;
    rsp.name_len         = strlen(s_device_name);
    rsp.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) { ESP_LOGW(TAG, "adv_rsp_set_fields rc=%d", rc); return; }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                           gap_event_handler, NULL);
    if (rc != 0) ESP_LOGW(TAG, "adv_start rc=%d", rc);
    else         ESP_LOGI(TAG, "advertising as '%s'", s_device_name);
}

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected (handle=%d)", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed status=%d; re-advertising", event->connect.status);
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=%d; re-advertising", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe attr=%u notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU negotiated = %d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer_auto rc=%d", rc); return; }
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset reason=%d", reason);
}

static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_nus_init(const char *device_name, ble_nus_rx_cb_t rx_cb)
{
    if (device_name && strlen(device_name) < sizeof(s_device_name)) {
        strcpy(s_device_name, device_name);
    }
    s_rx_cb = rx_cb;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", err);
        return err;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg rc=%d", rc); return ESP_FAIL; }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs rc=%d", rc); return ESP_FAIL; }

    ble_svc_gap_device_name_set(s_device_name);

    nimble_port_freertos_init(nimble_host_task);
    return ESP_OK;
}

void ble_nus_send(const char *s)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_tx_attr_handle == 0) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(s, strlen(s));
    if (!om) return;
    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_attr_handle, om);
    if (rc != 0 && rc != BLE_HS_ENOTCONN) {
        ESP_LOGW(TAG, "notify rc=%d", rc);
    }
}
