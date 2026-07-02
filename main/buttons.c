#include "buttons.h"
#include "gopro_control.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "buttons";

/* GPIO2/3/4/18 are general-purpose-safe pins on the ESP32-C6 Super
 * Mini (no bootstrapping/flash conflicts). */
#define BUTTON_SHUTTER GPIO_NUM_2
#define BUTTON_POWER   GPIO_NUM_3
#define BUTTON_MODE    GPIO_NUM_4
#define BUTTON_RESET   GPIO_NUM_18

static QueueHandle_t s_evt_queue;

static void IRAM_ATTR button_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)(uintptr_t)arg;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_evt_queue, &gpio_num, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

/* Debounces the given pin and blocks until it's released. Returns
 * false if it was just noise (pin already back high) rather than a
 * real press. */
static bool debounce_and_wait_release(gpio_num_t pin)
{
    vTaskDelay(pdMS_TO_TICKS(40));
    if (gpio_get_level(pin) != 0) {
        return false; /* bounce, not a real press */
    }
    while (gpio_get_level(pin) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

static void button_task(void *arg)
{
    uint32_t io_num;

    for (;;) {
        if (xQueueReceive(s_evt_queue, &io_num, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!debounce_and_wait_release((gpio_num_t)io_num)) {
            continue;
        }

        switch (io_num) {
        case BUTTON_SHUTTER:
            gopro_control_toggle_shutter();
            break;
        case BUTTON_POWER:
            gopro_control_power_button();
            break;
        case BUTTON_MODE:
            gopro_control_cycle_mode();
            break;
        case BUTTON_RESET:
            ESP_LOGW(TAG, "Physical reset button pressed");
            gopro_control_reset_device();
            break;
        default:
            break;
        }
    }
}

static void configure_button(gpio_num_t pin)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_isr_handler_add(pin, button_isr_handler, (void *)(uintptr_t)pin));
}

void buttons_init(void)
{
    s_evt_queue = xQueueCreate(8, sizeof(uint32_t));

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    configure_button(BUTTON_SHUTTER);
    configure_button(BUTTON_POWER);
    configure_button(BUTTON_MODE);
    configure_button(BUTTON_RESET);

    xTaskCreate(button_task, "buttons", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "Buttons ready: GPIO%d=shutter GPIO%d=power GPIO%d=mode GPIO%d=reset",
             BUTTON_SHUTTER, BUTTON_POWER, BUTTON_MODE, BUTTON_RESET);
}
