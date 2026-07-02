#include "app_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_mutex;
static conn_state_t s_conn = CONN_STATE_SCANNING;
static bool s_recording = false;
static gopro_preset_group_t s_mode = GOPRO_PRESET_VIDEO;

void app_state_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
}

void app_state_set_conn(conn_state_t state)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_conn = state;
    if (state != CONN_STATE_READY) {
        s_recording = false;
    }
    xSemaphoreGive(s_mutex);
}

conn_state_t app_state_get_conn(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    conn_state_t v = s_conn;
    xSemaphoreGive(s_mutex);
    return v;
}

void app_state_set_recording(bool recording)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_recording = recording;
    xSemaphoreGive(s_mutex);
}

bool app_state_get_recording(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool v = s_recording;
    xSemaphoreGive(s_mutex);
    return v;
}

void app_state_set_mode(gopro_preset_group_t mode)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_mode = mode;
    xSemaphoreGive(s_mutex);
}

gopro_preset_group_t app_state_get_mode(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    gopro_preset_group_t v = s_mode;
    xSemaphoreGive(s_mutex);
    return v;
}

const char *app_state_conn_str(conn_state_t state)
{
    switch (state) {
    case CONN_STATE_SCANNING:   return "scanning";
    case CONN_STATE_CONNECTING: return "connecting";
    case CONN_STATE_READY:      return "ready";
    default:                    return "unknown";
    }
}

const char *app_state_mode_str(gopro_preset_group_t mode)
{
    switch (mode) {
    case GOPRO_PRESET_VIDEO:     return "video";
    case GOPRO_PRESET_PHOTO:     return "photo";
    case GOPRO_PRESET_TIMELAPSE: return "timelapse";
    default:                     return "unknown";
    }
}
