#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "gopro_ble.h"
#include "app_state.h"
#include "led_status.h"
#include "buttons.h"
#include "version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Declared by the NimBLE "store/config" component; provides persistent
 * (NVS-backed) storage for bonding keys. No public header is shipped
 * for it in every IDF version, so it's forward-declared here exactly
 * like ESP-IDF's own blecent/bleprph examples do. */
void ble_store_config_init(void);

static const char *TAG = "main";

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
}

static void on_sync(void)
{
    /* Make sure we have a proper identity address before scanning. */
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed rc=%d", rc);
        return;
    }
    gopro_ble_scan();
}

static void nimble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    ESP_LOGI(TAG, "%s v%s starting", FIRMWARE_NAME, FIRMWARE_VERSION);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Central state store first - every other module reads/writes it. */
    app_state_init();

    /* Bring up the status LED and buttons before BLE starts, so the
     * LED shows "scanning" from the very first scan and the buttons
     * are ready the moment gopro_ble_scan() kicks off. */
    led_status_init();
    buttons_init();

    /* ESP-IDF >= 5.0: nimble_port_init() returns esp_err_t and takes
     * care of BT controller init internally (esp_nimble_hci_init() is
     * no longer called manually). Confirmed against ESP-IDF 5.5's
     * examples/bluetooth/nimble/blecent/main/main.c. */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE port, rc=%d", ret);
        return;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* --- Security parameters: Just Works bonding, LE Secure Connections ---
     * The GoPro has no display/keyboard for a passkey exchange, so we
     * use NoInputNoOutput (Just Works). Bonding must be enabled or the
     * GoPro will accept the GATT connection but ignore all commands. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    /* CONFIRMED FIX for this GoPro/ESP32-C6 pairing: use Legacy
     * Pairing, not LE Secure Connections. With sm_sc=1, the GoPro
     * terminated the connection (disconnect reason 531 / HCI 0x13)
     * every time security was requested, even after GATT discovery
     * settled the link and after narrowing key distribution to ENC
     * only. Switching to Legacy Pairing (sm_sc=0) fixed it - bonding
     * now completes and BLE_GAP_EVENT_ENC_CHANGE reports status=0. */
    ble_hs_cfg.sm_sc = 0;
    /* Only request LTK distribution (BLE_SM_PAIR_KEY_DIST_ENC). Also
     * requesting the identity key (BLE_SM_PAIR_KEY_DIST_ID, for IRK
     * distribution) has been observed to make the GoPro terminate the
     * connection right after a security request - this narrower set
     * matches a configuration confirmed to complete initial pairing
     * with a real GoPro camera. */
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;

    /* Persist bond info (LTK) to flash so we don't have to re-pair
     * with the GoPro after every reboot. */
    ble_store_config_init();

    gopro_ble_init();

    /* Note: we deliberately do NOT call ble_svc_gap_device_name_set()
     * here. That function lives in NimBLE's GAP *service* (the GATT
     * service a peripheral exposes to advertise its own name/
     * appearance), which is only compiled in when
     * CONFIG_BT_NIMBLE_GAP_SERVICE=y (itself tied to the peripheral
     * role). Since this app is central-only (we connect out to the
     * GoPro, we don't advertise ourselves), it's neither needed nor
     * linked in - calling it would fail with "undefined reference". */

    nimble_port_freertos_init(nimble_host_task);
}
