#ifndef GOPRO_BLE_H
#define GOPRO_BLE_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    GOPRO_PRESET_VIDEO,
    GOPRO_PRESET_PHOTO,
    GOPRO_PRESET_TIMELAPSE,
} gopro_preset_group_t;

/**
 * Registers GAP/GATT callbacks. Call once from app_main(),
 * BEFORE nimble_port_freertos_init().
 */
void gopro_ble_init(void);

/**
 * Starts scanning for a GoPro advertising the Control & Query
 * service (0xFEA6). Called automatically once the NimBLE host
 * syncs, but exposed in case you want to re-trigger a scan (e.g.
 * to wake a sleeping camera - see gopro_send_sleep()).
 */
void gopro_ble_scan(void);

/**
 * Sends the shutter command once connected, bonded and the
 * Control & Query characteristics have been discovered.
 * start = true  -> start recording / take photo
 * start = false -> stop recording
 */
void gopro_send_shutter(bool start);

/**
 * Puts the camera to sleep (Open GoPro command 0x05). The camera
 * disconnects itself right after. There is no separate "power on"
 * BLE command - a sleeping GoPro keeps advertising, and simply
 * connecting to it (which gopro_ble_scan() will do automatically,
 * including on the next reconnect cycle) wakes it back up.
 */
void gopro_send_sleep(void);

/**
 * Switches the camera to a different preset group (Video / Photo /
 * Timelapse), i.e. what most people think of as "changing mode".
 */
void gopro_load_preset_group(gopro_preset_group_t group);

/** Returns true once we're connected, bonded, and ready to send commands. */
bool gopro_is_ready(void);

#endif // GOPRO_BLE_H
