#include "gopro_control.h"
#include "gopro_ble.h"
#include "app_state.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "gopro_ctrl";

void gopro_control_toggle_shutter(void)
{
    if (!gopro_is_ready()) {
        ESP_LOGW(TAG, "Shutter requested but GoPro isn't ready - ignoring");
        return;
    }
    bool new_state = !app_state_get_recording();
    gopro_send_shutter(new_state);
    app_state_set_recording(new_state);
    ESP_LOGI(TAG, "Recording %s", new_state ? "STARTED" : "STOPPED");
}

void gopro_control_power_button(void)
{
    if (gopro_is_ready()) {
        ESP_LOGI(TAG, "Putting camera to sleep");
        gopro_send_sleep();
        app_state_set_recording(false);
    } else {
        /* Not connected: (re)start a scan. A sleeping GoPro is still
         * advertising, and connecting to it is what wakes it back up
         * - there's no separate "power on" BLE command. */
        ESP_LOGI(TAG, "Not connected - (re)starting scan to find/wake the camera");
        gopro_ble_scan();
    }
}

void gopro_control_cycle_mode(void)
{
    if (!gopro_is_ready()) {
        ESP_LOGW(TAG, "Mode change requested but GoPro isn't ready - ignoring");
        return;
    }
    gopro_preset_group_t current = app_state_get_mode();
    gopro_preset_group_t next = (current == GOPRO_PRESET_TIMELAPSE)
                                     ? GOPRO_PRESET_VIDEO
                                     : (gopro_preset_group_t)(current + 1);
    gopro_load_preset_group(next);
    app_state_set_mode(next);
}

static void reset_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

void gopro_control_reset_device(void)
{
    ESP_LOGW(TAG, "Reset requested - restarting in 300ms");
    xTaskCreate(reset_task, "reset_task", 2048, NULL, 5, NULL);
}
