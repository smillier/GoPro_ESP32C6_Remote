/*
 * gopro_ble.c
 *
 * Minimal NimBLE central implementation that:
 *   1. Scans for a GoPro advertising service 0xFEA6 (Control & Query)
 *   2. Connects to it
 *   3. Discovers the FEA6 service characteristics
 *   4. Subscribes (writes CCCD) on the notify characteristics
 *   5. THEN initiates BLE bonding (Just Works, LE Secure Connections)
 *      -> requesting bonding immediately on connect (before any GATT
 *         traffic) has been observed to make the GoPro terminate the
 *         link; doing GATT discovery first lets the connection settle.
 *         This is also what GoPro's own tutorials do: pairing is
 *         triggered only once the client starts touching protected
 *         characteristics, not proactively right after connecting.
 *   6. Exposes gopro_send_shutter() to fire the shutter
 *
 * Reference: https://gopro.github.io/OpenGoPro/ble/protocol/ble_setup.html
 *
 * Verified against ESP-IDF 5.5.4's NimBLE host APIs (ble_gap and
 * ble_gattc), matching examples/bluetooth/nimble/blecent in that
 * release.
 */

#include <string.h>
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "gopro_ble.h"
#include "app_state.h"

static const char *TAG = "gopro_ble";

/* ---- GoPro Control & Query service (16-bit) ---------------------- */
#define GOPRO_SVC_UUID16 0xFEA6

/* GATT Client Characteristic Configuration Descriptor */
#define CCCD_UUID16 0x2902

/* ---- GoPro 128-bit characteristic UUIDs --------------------------
 * Written here in the SAME byte order as the official UUID string
 * (b5f9XXXX-aa8d-11e3-9046-0002a5d5c51b), i.e. "big-endian" / how a
 * human reads it left to right. gopro_uuid128_from_be() reverses
 * these into the little-endian layout NimBLE expects internally, so
 * you can sanity-check these arrays directly against the Open GoPro
 * docs without doing the byte-swap by hand.
 * ------------------------------------------------------------------ */
typedef enum {
    GOPRO_CHR_COMMAND = 0,       /* 0x0072 - write            */
    GOPRO_CHR_COMMAND_RESP,      /* 0x0073 - notify           */
    GOPRO_CHR_SETTINGS,          /* 0x0074 - write             */
    GOPRO_CHR_SETTINGS_RESP,     /* 0x0075 - notify           */
    GOPRO_CHR_QUERY,             /* 0x0076 - write             */
    GOPRO_CHR_QUERY_RESP,        /* 0x0077 - notify           */
    GOPRO_CHR_COUNT
} gopro_chr_id_t;

static const uint8_t gopro_chr_uuid_be[GOPRO_CHR_COUNT][16] = {
    [GOPRO_CHR_COMMAND]       = {0xb5,0xf9,0x00,0x72,0xaa,0x8d,0x11,0xe3,0x90,0x46,0x00,0x02,0xa5,0xd5,0xc5,0x1b},
    [GOPRO_CHR_COMMAND_RESP]  = {0xb5,0xf9,0x00,0x73,0xaa,0x8d,0x11,0xe3,0x90,0x46,0x00,0x02,0xa5,0xd5,0xc5,0x1b},
    [GOPRO_CHR_SETTINGS]      = {0xb5,0xf9,0x00,0x74,0xaa,0x8d,0x11,0xe3,0x90,0x46,0x00,0x02,0xa5,0xd5,0xc5,0x1b},
    [GOPRO_CHR_SETTINGS_RESP] = {0xb5,0xf9,0x00,0x75,0xaa,0x8d,0x11,0xe3,0x90,0x46,0x00,0x02,0xa5,0xd5,0xc5,0x1b},
    [GOPRO_CHR_QUERY]         = {0xb5,0xf9,0x00,0x76,0xaa,0x8d,0x11,0xe3,0x90,0x46,0x00,0x02,0xa5,0xd5,0xc5,0x1b},
    [GOPRO_CHR_QUERY_RESP]    = {0xb5,0xf9,0x00,0x77,0xaa,0x8d,0x11,0xe3,0x90,0x46,0x00,0x02,0xa5,0xd5,0xc5,0x1b},
};

static const bool gopro_chr_is_notify[GOPRO_CHR_COUNT] = {
    [GOPRO_CHR_COMMAND]       = false,
    [GOPRO_CHR_COMMAND_RESP]  = true,
    [GOPRO_CHR_SETTINGS]      = false,
    [GOPRO_CHR_SETTINGS_RESP] = true,
    [GOPRO_CHR_QUERY]         = false,
    [GOPRO_CHR_QUERY_RESP]    = true,
};

typedef struct {
    uint16_t def_handle;
    uint16_t val_handle;
    uint16_t cccd_handle; /* 0 = not found / not applicable */
    bool     found;
} gopro_chr_state_t;

/* ---- module state --------------------------------------------------- */
/* s_gopro_addr already carries the peer's address type in its .type
 * field (copied straight from the discovery event), so we don't need
 * a separate address-type variable. */
static ble_addr_t s_gopro_addr;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_start_handle, s_svc_end_handle;
static gopro_chr_state_t s_chrs[GOPRO_CHR_COUNT];
static int s_dsc_target = -1; /* which s_chrs[] index we're currently discovering descriptors for */
static bool s_ready = false;

static int gopro_gap_event(struct ble_gap_event *event, void *arg);

/* ---- helpers ---------------------------------------------------------- */

static void gopro_uuid128_from_be(ble_uuid128_t *out, const uint8_t be[16])
{
    out->u.type = BLE_UUID_TYPE_128;
    for (int i = 0; i < 16; i++) {
        out->value[i] = be[15 - i];
    }
}

static bool uuid_matches_chr(const ble_uuid_t *uuid, gopro_chr_id_t id)
{
    ble_uuid128_t want;
    gopro_uuid128_from_be(&want, gopro_chr_uuid_be[id]);
    return ble_uuid_cmp(uuid, &want.u) == 0;
}

static int chr_id_for_uuid(const ble_uuid_t *uuid)
{
    for (int i = 0; i < GOPRO_CHR_COUNT; i++) {
        if (uuid_matches_chr(uuid, (gopro_chr_id_t)i)) {
            return i;
        }
    }
    return -1;
}

bool gopro_is_ready(void)
{
    return s_ready;
}

/* ---- Step 6: subscribe to notify characteristics one at a time ------ */

static void gopro_discover_next_dsc(void);

static int gopro_on_dsc_disc(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              uint16_t chr_val_handle,
                              const struct ble_gatt_dsc *dsc,
                              void *arg)
{
    if (s_dsc_target < 0) {
        return 0;
    }

    if (error->status == 0 && dsc != NULL) {
        if (ble_uuid_u16(&dsc->uuid.u) == CCCD_UUID16) {
            s_chrs[s_dsc_target].cccd_handle = dsc->handle;
        }
        return 0;
    }

    /* status == BLE_HS_EDONE -> finished this characteristic's descriptors */
    if (s_chrs[s_dsc_target].cccd_handle != 0) {
        uint8_t enable_notify[2] = {0x01, 0x00};
        int rc = ble_gattc_write_flat(conn_handle,
                                       s_chrs[s_dsc_target].cccd_handle,
                                       enable_notify, sizeof(enable_notify),
                                       NULL, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "CCCD write failed for chr %d, rc=%d", s_dsc_target, rc);
        } else {
            ESP_LOGI(TAG, "Subscribed to notifications on characteristic %d", s_dsc_target);
        }
    } else {
        ESP_LOGW(TAG, "No CCCD found for notify characteristic %d", s_dsc_target);
    }

    s_dsc_target++;
    gopro_discover_next_dsc();
    return 0;
}

static void gopro_discover_next_dsc(void)
{
    while (s_dsc_target < GOPRO_CHR_COUNT &&
           (!s_chrs[s_dsc_target].found || !gopro_chr_is_notify[s_dsc_target])) {
        s_dsc_target++;
    }

    if (s_dsc_target >= GOPRO_CHR_COUNT) {
        ESP_LOGI(TAG, "Discovery + subscriptions complete. Requesting bonding now...");
        /* NimBLE (unlike Bluedroid) initiates security "on demand"
         * rather than immediately on connect. Requesting it right
         * after the connection completes - before any GATT traffic -
         * has been observed to make the GoPro terminate the link
         * (disconnect reason 531 / HCI 0x13, "Remote User
         * Terminated"). Waiting until after service/characteristic
         * discovery (a few round trips) lets the link settle first. */
        int rc = ble_gap_security_initiate(s_conn_handle);
        if (rc != 0) {
            ESP_LOGE(TAG, "security_initiate failed rc=%d", rc);
        }
        return;
    }

    uint16_t start = s_chrs[s_dsc_target].val_handle + 1;
    uint16_t end = s_svc_end_handle;
    /* narrow the range to just before the next known characteristic, if any */
    for (int i = 0; i < GOPRO_CHR_COUNT; i++) {
        if (s_chrs[i].found && s_chrs[i].def_handle > s_chrs[s_dsc_target].def_handle) {
            if (s_chrs[i].def_handle - 1 < end) {
                end = s_chrs[i].def_handle - 1;
            }
        }
    }

    if (start > end) {
        /* No room for any descriptor (back-to-back characteristics
         * with no CCCD) - move on. */
        ESP_LOGW(TAG, "No descriptor space for chr %d, skipping", s_dsc_target);
        s_dsc_target++;
        gopro_discover_next_dsc();
        return;
    }

    if (start == end) {
        /* Exactly one handle between this characteristic and the
         * next - NimBLE's disc_all_dscs rejects a zero-width range
         * (start==end) with EINVAL, but in practice that single
         * handle is the CCCD (the only descriptor GoPro's notify
         * characteristics have), so just use it directly instead of
         * running a discovery procedure for it. */
        s_chrs[s_dsc_target].cccd_handle = start;
        uint8_t enable_notify[2] = {0x01, 0x00};
        int rc = ble_gattc_write_flat(s_conn_handle, start,
                                       enable_notify, sizeof(enable_notify),
                                       NULL, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "CCCD write failed for chr %d, rc=%d", s_dsc_target, rc);
        } else {
            ESP_LOGI(TAG, "Subscribed to notifications on characteristic %d", s_dsc_target);
        }
        s_dsc_target++;
        gopro_discover_next_dsc();
        return;
    }

    int rc = ble_gattc_disc_all_dscs(s_conn_handle, start, end, gopro_on_dsc_disc, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "disc_all_dscs failed rc=%d, skipping chr %d", rc, s_dsc_target);
        s_dsc_target++;
        gopro_discover_next_dsc();
    }
}

/* ---- Step 5: discover characteristics of the FEA6 service ------------ */

static int gopro_on_chr_disc(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              const struct ble_gatt_chr *chr,
                              void *arg)
{
    if (error->status == 0 && chr != NULL) {
        int id = chr_id_for_uuid(&chr->uuid.u);
        if (id >= 0) {
            s_chrs[id].def_handle = chr->def_handle;
            s_chrs[id].val_handle = chr->val_handle;
            s_chrs[id].found = true;
            ESP_LOGI(TAG, "Found characteristic %d (val handle 0x%04x)", id, chr->val_handle);
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Characteristic discovery complete, subscribing to notifications...");
        s_dsc_target = 0;
        gopro_discover_next_dsc();
    } else {
        ESP_LOGE(TAG, "Characteristic discovery error, status=%d", error->status);
    }
    return 0;
}

/* ---- Step 4: discover the FEA6 service -------------------------------- */

static int gopro_on_svc_disc(uint16_t conn_handle,
                              const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service,
                              void *arg)
{
    if (error->status == BLE_HS_EDONE) {
        /* Normal end-of-procedure signal (we've already processed the
         * one matching service below, on the status==0 callback) -
         * not an error, nothing to do here. */
        return 0;
    }

    if (error->status != 0 || service == NULL) {
        ESP_LOGE(TAG, "Service discovery failed, status=%d", error->status);
        return 0;
    }

    s_svc_start_handle = service->start_handle;
    s_svc_end_handle = service->end_handle;

    int rc = ble_gattc_disc_all_chrs(conn_handle,
                                       service->start_handle,
                                       service->end_handle,
                                       gopro_on_chr_disc, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "disc_all_chars failed rc=%d", rc);
    }
    return 0;
}

/* ---- Step 3: connected -> start discovery (bonding requested after) --- */

static void gopro_start_discovery(void)
{
    ble_uuid16_t svc_uuid = BLE_UUID16_INIT(GOPRO_SVC_UUID16);
    memset(s_chrs, 0, sizeof(s_chrs));
    int rc = ble_gattc_disc_svc_by_uuid(s_conn_handle, &svc_uuid.u, gopro_on_svc_disc, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "disc_svc_by_uuid failed rc=%d", rc);
    }
}

/* ---- Step 2: connect (bonding is requested later, after discovery) ---- */

static void gopro_connect(void)
{
    /* IMPORTANT: the first argument to ble_gap_connect() is the type of
     * address WE (the ESP32) advertise/connect with — NOT the GoPro's
     * address type. The peer's address type is carried inside
     * s_gopro_addr (a ble_addr_t, set from event->disc.addr in the
     * scan callback), which already has its own .type field. */
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed rc=%d", rc);
        return;
    }

    struct ble_gap_conn_params params = {0};
    /* 100% duty-cycle background scan while connecting, for the
     * fastest possible connection to the GoPro. (This was previously
     * throttled to ~50% to coexist with a WiFi SoftAP that has since
     * been removed from this firmware.) */
    params.scan_itvl = 0x0010;
    params.scan_window = 0x0010;
    params.itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN;
    params.itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX;
    params.latency = 0;
    params.supervision_timeout = 256;
    params.min_ce_len = 0x0010;
    params.max_ce_len = 0x0300;

    rc = ble_gap_connect(own_addr_type, &s_gopro_addr, 30000,
                          &params, gopro_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect failed rc=%d", rc);
    }
}

/* ---- Step 1: scanning / advertisement filtering ------------------------ */

static bool adv_has_gopro_service(const struct ble_hs_adv_fields *fields)
{
    for (int i = 0; i < fields->num_uuids16; i++) {
        if (ble_uuid_u16(&fields->uuids16[i].u) == GOPRO_SVC_UUID16) {
            return true;
        }
    }
    return false;
}

void gopro_ble_scan(void)
{
    struct ble_gap_disc_params disc_params = {0};
    disc_params.filter_duplicates = 1;
    disc_params.passive = 0;
    disc_params.itvl = 0x0010;
    disc_params.window = 0x0010;
    disc_params.filter_policy = 0;
    disc_params.limited = 0;

    ESP_LOGI(TAG, "Scanning for GoPro (put the camera into pairing mode)...");
    app_state_set_conn(CONN_STATE_SCANNING);
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, 30000, &disc_params, gopro_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed rc=%d", rc);
    }
}

/* ---- master GAP event handler ------------------------------------------ */

static int gopro_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_hs_adv_fields fields;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
            return 0;
        }
        if (adv_has_gopro_service(&fields)) {
            ESP_LOGI(TAG, "Found GoPro advertisement, connecting...");
            ble_gap_disc_cancel();
            s_gopro_addr = event->disc.addr;
            gopro_connect();
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "Connected. Discovering GATT database (bonding requested after)...");
            app_state_set_conn(CONN_STATE_CONNECTING);
            s_conn_handle = event->connect.conn_handle;
            gopro_start_discovery();
        } else {
            ESP_LOGE(TAG, "Connection failed, status=%d, rescanning", event->connect.status);
            gopro_ble_scan();
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "Link encrypted/bonded. GoPro is ready for commands.");
            s_ready = true;
            app_state_set_conn(CONN_STATE_READY);
        } else {
            ESP_LOGE(TAG, "Encryption/bonding failed, status=%d", event->enc_change.status);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        ESP_LOGI(TAG, "Notification on handle 0x%04x, %d bytes",
                 event->notify_rx.attr_handle,
                 OS_MBUF_PKTLEN(event->notify_rx.om));
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGI(TAG, "Scan window ended without finding the GoPro, restarting scan...");
            gopro_ble_scan();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "Disconnected, reason=%d. Rescanning...", event->disconnect.reason);
        app_state_set_conn(CONN_STATE_SCANNING);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_ready = false;
        gopro_ble_scan();
        return 0;

    default:
        return 0;
    }
}

void gopro_ble_init(void)
{
    /* Nothing to register up front here; the sync_cb in main.c calls
     * gopro_ble_scan() once the NimBLE host is ready. Kept as a
     * separate init function in case you want to add persistent
     * state / NVS reads here later. */
    ESP_LOGI(TAG, "gopro_ble module initialized");
}

/* ---- public command API ------------------------------------------------ */

void gopro_send_shutter(bool start)
{
    if (!s_ready || !s_chrs[GOPRO_CHR_COMMAND].found) {
        ESP_LOGW(TAG, "Not ready yet, ignoring shutter command");
        return;
    }

    /* GoPro command TLV: [length][command_id][param_id][param_len][param_value...]
     * Shutter command_id = 0x01, single param (0x01) = start(1)/stop(0) */
    uint8_t payload[4] = {0x03, 0x01, 0x01, start ? 0x01 : 0x00};

    int rc = ble_gattc_write_flat(s_conn_handle,
                                   s_chrs[GOPRO_CHR_COMMAND].val_handle,
                                   payload, sizeof(payload),
                                   NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "shutter write failed rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Shutter %s command sent", start ? "START" : "STOP");
    }
}

void gopro_send_sleep(void)
{
    if (!s_ready || !s_chrs[GOPRO_CHR_COMMAND].found) {
        ESP_LOGW(TAG, "Not ready yet, ignoring sleep command");
        return;
    }

    /* Sleep = command ID 0x05, no parameters.
     * TLV: [length=1][command_id=0x05] */
    uint8_t payload[2] = {0x01, 0x05};

    int rc = ble_gattc_write_flat(s_conn_handle,
                                   s_chrs[GOPRO_CHR_COMMAND].val_handle,
                                   payload, sizeof(payload),
                                   NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "sleep write failed rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Sleep command sent - camera will power down and disconnect");
    }
}

void gopro_load_preset_group(gopro_preset_group_t group)
{
    if (!s_ready || !s_chrs[GOPRO_CHR_COMMAND].found) {
        ESP_LOGW(TAG, "Not ready yet, ignoring preset group change");
        return;
    }

    /* Load Preset Group = command ID 0x3E, one 16-bit big-endian
     * parameter carrying the EnumPresetGroup value:
     *   1000 (0x03E8) = Video
     *   1001 (0x03E9) = Photo
     *   1002 (0x03EA) = Timelapse
     * TLV: [length=4][command_id=0x3E][param_len=2][param value hi][lo] */
    uint16_t group_id;
    const char *name;
    switch (group) {
    case GOPRO_PRESET_VIDEO:     group_id = 1000; name = "Video";     break;
    case GOPRO_PRESET_PHOTO:     group_id = 1001; name = "Photo";     break;
    case GOPRO_PRESET_TIMELAPSE: group_id = 1002; name = "Timelapse"; break;
    default:
        ESP_LOGW(TAG, "Unknown preset group %d", group);
        return;
    }

    uint8_t payload[5] = {
        0x04, 0x3E, 0x02,
        (uint8_t)(group_id >> 8), (uint8_t)(group_id & 0xFF)
    };

    int rc = ble_gattc_write_flat(s_conn_handle,
                                   s_chrs[GOPRO_CHR_COMMAND].val_handle,
                                   payload, sizeof(payload),
                                   NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "load preset group write failed rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "Load Preset Group -> %s sent", name);
    }
}
