idf_component_register(
    SRCS "main.c" "gopro_ble.c" "app_state.c" "gopro_control.c"
         "led_status.c" "buttons.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash bt esp_timer driver
)
