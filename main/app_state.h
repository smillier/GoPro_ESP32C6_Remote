#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdbool.h>
#include "gopro_ble.h"

typedef enum {
    CONN_STATE_SCANNING,
    CONN_STATE_CONNECTING,
    CONN_STATE_READY,
} conn_state_t;

/** Call once from app_main(), before any other module reads/writes state. */
void app_state_init(void);

/* Connection state. Leaving CONN_STATE_READY automatically clears the
 * recording flag (the camera can't be recording if we're not even
 * connected to it), so callers never have to remember to do that
 * themselves on disconnect. */
void app_state_set_conn(conn_state_t state);
conn_state_t app_state_get_conn(void);

void app_state_set_recording(bool recording);
bool app_state_get_recording(void);

void app_state_set_mode(gopro_preset_group_t mode);
gopro_preset_group_t app_state_get_mode(void);

/* Human-readable helpers - used by the web UI's JSON status and logs. */
const char *app_state_conn_str(conn_state_t state);
const char *app_state_mode_str(gopro_preset_group_t mode);

#endif // APP_STATE_H
