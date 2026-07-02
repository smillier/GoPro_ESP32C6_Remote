#include "led_status.h"
#include "app_state.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "led_status";

/* WS2812 RGB LED on the ESP32-C6 Super Mini board. */
#define LED_GPIO 8

/* Keep brightness low - these are quite bright at full 0-255 values
 * and this only needs to be a status indicator. */
#define LED_BRIGHTNESS 40

static led_strip_handle_t s_strip;

static void led_task(void *arg)
{
    bool blink_on = true;

    while (1) {
        conn_state_t conn = app_state_get_conn();
        bool recording = app_state_get_recording();
        uint8_t r = 0, g = 0, b = 0;

        if (recording) {
            /* Recording overrides the connection-state color. */
            if (blink_on) {
                r = LED_BRIGHTNESS;
            }
        } else {
            switch (conn) {
            case CONN_STATE_SCANNING:
                if (blink_on) {
                    b = LED_BRIGHTNESS;
                }
                break;
            case CONN_STATE_CONNECTING:
                r = LED_BRIGHTNESS;
                g = LED_BRIGHTNESS * 3 / 4;
                break;
            case CONN_STATE_READY:
                g = LED_BRIGHTNESS;
                break;
            }
        }

        led_strip_set_pixel(s_strip, 0, r, g, b);
        led_strip_refresh(s_strip);

        blink_on = !blink_on;
        vTaskDelay(pdMS_TO_TICKS(400));
    }
}

void led_status_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz */
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    led_strip_clear(s_strip);

    xTaskCreate(led_task, "led_status", 2048, NULL, 3, NULL);
    ESP_LOGI(TAG, "WS2812 status LED ready on GPIO%d", LED_GPIO);
}
